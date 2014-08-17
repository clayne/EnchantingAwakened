#include "skse/PluginAPI.h"
#include "skse/skse_version.h"
#include "skse/ScaleformCallbacks.h"
#include "skse/ScaleformMovie.h"
#include "skse/GameAPI.h"
//#include "skse/SafeWrite.h"

#include "skse/GameForms.h"
#include "skse/GameObjects.h"
#include "skse/GameFormComponents.h"
#include "skse/GameExtraData.h"
#include "skse/GameBSExtraData.h"
#include "skse/GameRTTI.h"
#include "skse/GameData.h"

#include "skse/PapyrusArgs.h"
#include "skse/GameObjects.h"
#include "skse/GameRTTI.h"
#include "skse/PapyrusVM.h"
#include "skse/PapyrusNativeFunctions.h"

#include <shlobj.h>
#include <vector>


IDebugLog						gLog;
const char*						kLogPath = "\\My Games\\Skyrim\\Logs\\EA_Extender.log";

PluginHandle	g_pluginHandle = kPluginHandle_Invalid;

SKSEScaleformInterface		* g_scaleform = NULL;
SKSESerializationInterface	* g_serialization = NULL;
SKSEPapyrusInterface   *g_papyrus = NULL;

class VMClassRegistry;
class VMValue;
struct StaticFunctionTag;



class MagicSkillStrings
{
public:
	static MagicSkillStrings& Instance()
	{
		static MagicSkillStrings instance;
		return instance;
	}

	BSFixedString LookupSkillString(UInt32 skillNumber)
	{
		switch(skillNumber)
		{
			case 18:	return Alteration;
			case 19:	return Conjuration;
			case 20:	return Destruction;
			case 21:	return Illusion;
			case 22:	return Restoration;
			default:	return NullString;
		}
	}

	BSFixedString Alteration;
	BSFixedString Conjuration;
	BSFixedString Destruction;
	BSFixedString Illusion;
	BSFixedString Restoration;
	BSFixedString NullString;

private:
	MagicSkillStrings() :
		Alteration("Alteration"),
		Conjuration("Conjuration"),
		Destruction("Destruction"),
		Illusion("Illusion"),
		Restoration("Restoration"),
		NullString("") {}
};

MagicSkillStrings magicSkillStrings = MagicSkillStrings::Instance();



char* GetFunctionName(UInt32 functionID);


class MatchByEquipSlot : public FormMatcher
{
	UInt32 m_mask;
	UInt32 m_hand;
	Actor * m_actor;
public:
	MatchByEquipSlot(Actor * actor, UInt32 hand, UInt32 slot) : 
	  m_hand(hand),
	  m_mask(slot),
	  m_actor(actor)
	  {

	  }

	  enum
	  {
		  kSlotID_Left = 0,
		  kSlotID_Right
	  };

	  bool Matches(TESForm* pForm) const {
		  if (pForm) {
			  if(pForm->formType != TESObjectWEAP::kTypeID) { // If not a weapon use mask
				  BGSBipedObjectForm* pBip = DYNAMIC_CAST(pForm, TESForm, BGSBipedObjectForm);
				  if (pBip)
					  return (pBip->data.parts & m_mask) != 0;
			  } else if(m_mask == 0) { // Use hand if no mask specified
				  TESForm * equippedForm = m_actor->GetEquippedObject(m_hand == kSlotID_Left);
				  return (equippedForm && equippedForm == pForm);
			  }
		  }
		  return false;
	  }
};

namespace utilFuncs
{
	BSFixedString GetDisplayName(TESForm* baseForm, BaseExtraList* extraData)
	{
		return (baseForm && extraData) ? extraData->GetDisplayName(baseForm) : "";
	}

	EquipData ResolveEquippedObject(Actor * actor, UInt32 weaponSlot, UInt32 slotMask)
	{
		EquipData foundData;
		foundData.pForm = NULL;
		foundData.pExtraData = NULL;
		if(!actor)
			return foundData;

		MatchByEquipSlot matcher(actor, weaponSlot, slotMask);
		ExtraContainerChanges* pContainerChanges = static_cast<ExtraContainerChanges*>(actor->extraData.GetByType(kExtraData_ContainerChanges));
		if (pContainerChanges) {
			foundData = pContainerChanges->FindEquipped(matcher, weaponSlot == MatchByEquipSlot::kSlotID_Right, weaponSlot == MatchByEquipSlot::kSlotID_Left);
			return foundData;
		}

		return foundData;
	}

	EnchantmentItem* GetEnchantment(BaseExtraList * extraData)
	{
		if (!extraData)
			return NULL;

		ExtraEnchantment* extraEnchant = static_cast<ExtraEnchantment*>(extraData->GetByType(kExtraData_Enchantment));
		return extraEnchant ? extraEnchant->enchant : NULL;
	}

	EnchantmentItem* GetExtraEnchantment(Actor* actor, TESObjectWEAP* pWeap)
	{
		if (!actor || !pWeap)
			return NULL;

		TESForm* eqForm = DYNAMIC_CAST(pWeap, TESObjectWEAP, TESForm);
		UInt32 equipSlot = ((actor->GetEquippedObject(false)) == eqForm) ? 1 : 0; //find slot eqForm was equipped into
		UInt32 wornSlot = 0;

		EquipData equipData = ResolveEquippedObject(actor, equipSlot, wornSlot);
		if(equipData.pForm && equipData.pExtraData)
			return GetEnchantment(equipData.pExtraData);

		return NULL;
	}

	UInt32 CalcMagicItemCost(MagicItem* magItem)
	{
		if (!magItem)
			return 0;

		UInt32 totalCost = 0;
		for (UInt32 i = 0; i < magItem->effectItemList.count; i++)
		{
			MagicItem::EffectItem* pEffectItem = NULL;
			magItem->effectItemList.GetNthItem(i, pEffectItem);
			if (pEffectItem)
				totalCost += (UInt32)pEffectItem->cost;
		}
		return totalCost;
	}
}


bool KeyListHasMagicDisallowEnchanting(BGSKeywordForm* keywords)
{
	if (!keywords)
		return false;

	static BGSKeyword* magicDisallowEnchanting = DYNAMIC_CAST(LookupFormByID(0x000C27BD), TESForm, BGSKeyword);

	for(UInt32 k = 0; k < keywords->numKeywords; ++k)
		if (keywords->keywords[k] == magicDisallowEnchanting)
			return true;
	return false;
}

bool FormHasMagicDisallowEnchanting(TESForm* form)
{
	BGSKeywordForm* keywords = DYNAMIC_CAST(form, TESForm, BGSKeywordForm);
	return KeyListHasMagicDisallowEnchanting(keywords);
}

inline bool IsPlayerCraftedEnchantment(EnchantmentItem* enchantment)
{
	return (enchantment) ? (enchantment->formID >= 0xFF000000) : false;
}




struct FormEnchantmentPair
{
	TESForm*			form;
	EnchantmentItem*	enchantment;
	FormEnchantmentPair(TESForm* f, EnchantmentItem* e) : form(f), enchantment(e) {}
};

typedef std::vector<FormEnchantmentPair> FormEnchantmentVec;

class ExtraContainerEnchantedItemExtractor
{
public:
	ExtraContainerEnchantedItemExtractor(TESObjectREFR* reference) : enchantedItemVec()
	{
		enchantedItemVec.clear();
		ExtraContainerChanges* pXContainerChanges = static_cast<ExtraContainerChanges*>(reference->extraData.GetByType(kExtraData_ContainerChanges));
		containerList = (pXContainerChanges) ? pXContainerChanges->data->objList : NULL;
	}

	bool GetExtraEnchantedForms(VMArray<TESForm*>* fillForms, VMArray<EnchantmentItem*>* fillEnchantments, UInt32 startIndex, bool excludePlayerEnchants, bool excludeDisallowEnchanting)
	{
		if (!containerList)
			return false;

		if (enchantedItemVec.size() == 0)
			containerList->Visit(*this);

		FormEnchantmentVec::iterator it = enchantedItemVec.begin();
		for (UInt32 i = startIndex; (i < fillForms->Length()) && (it != enchantedItemVec.end()); ++it)
		{
			if (excludeDisallowEnchanting && FormHasMagicDisallowEnchanting(it->form)) continue;
			if (excludePlayerEnchants && IsPlayerCraftedEnchantment(it->enchantment)) continue;

			for (UInt32 limit = 3; it->enchantment->data.baseEnchantment && (limit > 0); limit--)
				it->enchantment = it->enchantment->data.baseEnchantment;

			fillForms->Set(&it->form, i);
			fillEnchantments->Set(&it->enchantment, i);

			++i;
		}

		return true;
	}

	bool Accept(ExtraContainerChanges::EntryData* data) 
	{
		if (data && (data->countDelta > 0))
		{
			TESForm* dataForm = data->type;

			if (TESObjectARMO* dataArmor = DYNAMIC_CAST(dataForm, TESForm, TESObjectARMO))
			{
				if (dataArmor->enchantable.enchantment)
					enchantedItemVec.push_back(FormEnchantmentPair(dataForm, dataArmor->enchantable.enchantment));
			}
			else if (TESObjectWEAP* dataWeapon = DYNAMIC_CAST(dataForm, TESForm, TESObjectWEAP))
			{
				if (dataWeapon->enchantable.enchantment)
					enchantedItemVec.push_back(FormEnchantmentPair(dataForm, dataWeapon->enchantable.enchantment));
			}
		}
		return true;
	}

private:
	ExtraContainerChanges::EntryDataList*	containerList;
	FormEnchantmentVec						enchantedItemVec;
};



// class KeywordCollection
// {
// public:
// 	KeywordCollection(VMArray<TESForm*>& forms, UInt32 num) : keys()
// 	{
// 		for(UInt32 i = 0; i < forms->Length(); i++)
// 		{
// 			TESForm* thisForm = NULL;
// 			forms->Get(&thisForm, i);
// 			if (!thisForm) //terminate function when NULL entry is found (should signal end of array contents)
// 				break;

// 			BGSKeywordForm* pKeywords = DYNAMIC_CAST(thisForm, TESForm, BGSKeywordForm);
// 			if (pKeywords && num < pKeywords->numKeywords)
// 				keys.push_back(pKeywords->keywords[num]);
// 		}
// 	}

// private:
// 	std::vector<BGSKeyword*>	keys;
// };



namespace papyrusEAExtender
{
	void SetNthKeyword(StaticFunctionTag* base, TESForm* thisForm, UInt32 index, BGSKeyword* newKeywordToSet)
	{
		if (!thisForm)
			return;

		BGSKeywordForm* pKeywords = DYNAMIC_CAST(thisForm, TESForm, BGSKeywordForm);
		if (pKeywords && index < pKeywords->numKeywords)
			pKeywords->keywords[index] = newKeywordToSet;
	}

	void SetFormArrayNthKeyword(StaticFunctionTag* base, VMArray<TESForm*> inputForms, UInt32 index, BGSKeyword* newKeywordToSet)
	{
		for(UInt32 i = 0; i < inputForms.Length(); i++)
		{
			TESForm* thisForm = NULL;
			inputForms.Get(&thisForm, i);
			if (!thisForm) //ternminate function when NULL entry is found (should signal end of array contents)
				return;

			BGSKeywordForm* pKeywords = DYNAMIC_CAST(thisForm, TESForm, BGSKeywordForm);
			if (pKeywords && index < pKeywords->numKeywords)
				pKeywords->keywords[index] = newKeywordToSet;
		}
	}

	void SetFormArrayNthKeywordArray(StaticFunctionTag* base, VMArray<TESForm*> inputForms, UInt32 index, VMArray<BGSKeyword*> newKeywordsToSet)
	{
		if(inputForms.Length() > newKeywordsToSet.Length()) //only process if we've got enough keywords
			return;

		for(UInt32 i = 0; i < inputForms.Length(); i++)
		{
			TESForm* thisForm = NULL;
			inputForms.Get(&thisForm, i);
			if (!thisForm) //End of array
				return;

			BGSKeywordForm* pKeywords = DYNAMIC_CAST(thisForm, TESForm, BGSKeywordForm);
			if (pKeywords && index < pKeywords->numKeywords)
			{
				BGSKeyword* newKeyword = NULL;
				newKeywordsToSet.Get(&newKeyword, i);
				if (newKeyword)
					pKeywords->keywords[index] = newKeyword;
			}
		}
	}

	void DumpEnchantmentValues(EnchantmentItem* pEnch); //forward declaration
	void GetFormArrayNthKeywords(StaticFunctionTag* base, VMArray<TESForm*> inputForms, UInt32 index, VMArray<BGSKeyword*> fillKeys)
	{
		if (inputForms.Length() > fillKeys.Length())
			return;

		for (UInt32 i = 0; i < inputForms.Length(); ++i)
		{
			TESForm* thisForm = NULL;
			inputForms.Get(&thisForm, i);
			if (!thisForm) //End of array
				return;

			BGSKeywordForm* pKeywords = DYNAMIC_CAST(thisForm, TESForm, BGSKeywordForm);
			BGSKeyword* setKey = (pKeywords && index < pKeywords->numKeywords) ? pKeywords->keywords[index] : NULL;
			fillKeys.Set(&setKey, i);
		}
	}


	bool GetEnchantedForms(StaticFunctionTag* base, TESObjectREFR* pContainerRef, VMArray<TESForm*> forms, VMArray<EnchantmentItem*> enchantments, bool excludePlayerEnchants, bool excludeDisallowEnchanting)
	{
		if (!pContainerRef || forms.Length() != enchantments.Length())
			return false;

		TESContainer* pContainer = NULL;
		TESForm* pBaseForm = pContainerRef->baseForm;
		pContainer = (pBaseForm) ? DYNAMIC_CAST(pBaseForm, TESForm, TESContainer) : NULL;
		if (!pContainer)
			return false;

		UInt32 fillIndex = 0;

		if (pContainer)
		{
			for (UInt32 i = 0; i < pContainer->numEntries; ++i)
			{
				TESForm* thisForm = (pContainer->entries[i]->count) ? pContainer->entries[i]->form : NULL;
				if (!thisForm)
					continue;

				EnchantmentItem* thisEnchantment = NULL;
				TESObjectARMO* thisArmor = DYNAMIC_CAST(thisForm, TESForm, TESObjectARMO);
				if (thisArmor)
					thisEnchantment = thisArmor->enchantable.enchantment;
				else
				{
					TESObjectWEAP* thisWeapon = DYNAMIC_CAST(thisForm, TESForm, TESObjectWEAP);
					if (thisWeapon)
						thisEnchantment = thisWeapon->enchantable.enchantment;
				}

				if (!thisEnchantment) continue;
				if (excludeDisallowEnchanting && FormHasMagicDisallowEnchanting(thisForm)) continue;
				if (excludePlayerEnchants && IsPlayerCraftedEnchantment(thisEnchantment)) continue;

				for (UInt32 limit = 3; thisEnchantment->data.baseEnchantment && (limit > 0); limit--)
					thisEnchantment = thisEnchantment->data.baseEnchantment;

				forms.Set(&thisForm, fillIndex);
				enchantments.Set(&thisEnchantment, fillIndex);
				++fillIndex;
			}
		}

		//Check the extra data too (I don't think this is really necessary for player inventory, but just in case)
		ExtraContainerEnchantedItemExtractor extraEnchantedItems(pContainerRef);
		extraEnchantedItems.GetExtraEnchantedForms(&forms, &enchantments, fillIndex, excludePlayerEnchants, excludeDisallowEnchanting);
		return true;
	}


	//Enchanting Awakened function to check if form is enchanted, and return specific data about it.
	bool CheckFormForEnchantment(StaticFunctionTag* base, TESForm* form, VMArray<TESForm*> returnData)
	{
		EnchantmentItem* enchantment = NULL;

		if (TESObjectARMO* armor = DYNAMIC_CAST(form, TESForm, TESObjectARMO))
			enchantment = armor->enchantable.enchantment;
		else if (TESObjectWEAP* weapon = DYNAMIC_CAST(form, TESForm, TESObjectWEAP))
			enchantment = weapon->enchantable.enchantment;

		if (!enchantment || IsPlayerCraftedEnchantment(enchantment) || returnData.Length() < 3)
			return false;

		BGSKeywordForm* keys = DYNAMIC_CAST(form, TESForm, BGSKeywordForm);
		if (!keys || keys->numKeywords < 1)
			return false;

		if (KeyListHasMagicDisallowEnchanting(keys))
			return false;

		for (UInt32 limit = 3; enchantment->data.baseEnchantment && (limit > 0); limit--)
			enchantment = enchantment->data.baseEnchantment;

		TESForm* enchantmentForm = DYNAMIC_CAST(enchantment, EnchantmentItem, TESForm);
		TESForm* keywordForm = keys->keywords[0];

		returnData.Set(&form, 0);
		returnData.Set(&enchantmentForm, 1);
		returnData.Set(&keywordForm, 2);
		return true;
	}


	void GetFormNames(StaticFunctionTag* base, VMArray<TESForm*> inputForms, VMArray<BSFixedString> returnStrings)
	{
		if (returnStrings.Length() < inputForms.Length())
			return;

		for (UInt32 i = 0; i < inputForms.Length(); ++i)
		{
			TESForm* thisForm = NULL;
			inputForms.Get(&thisForm, i);
			if (!thisForm) //terminate when NULL entry is found (should signal end of array)
				return;

			TESFullName* thisName = DYNAMIC_CAST(thisForm, TESForm, TESFullName);
			if (!thisName)
				return;

			returnStrings.Set(&thisName->name, i);
		}
	}


	bool IsSpellSkillType(StaticFunctionTag* base, SpellItem* spell, BSFixedString skillType)
	{
		if (!skillType)
			return false;

		UInt32 school = LookupActorValueByName(skillType.data);
		if (!spell || spell->data.type != 0x00 || school < 18 || school > 22) //0x00 == "Spell" (ignore voice/ability/disease/etc)
			return false;

		for (UInt32 i = 0; i < spell->effectItemList.count; ++i)
		{
			MagicItem::EffectItem* effect = NULL;
			spell->effectItemList.GetNthItem(i, effect);
			if (effect && effect->mgef->properties.school == school)
				return true;
		}
		return false;
	}


	BSFixedString GetSpellSkillString(StaticFunctionTag* base, SpellItem* spell)
	{
		if (!spell || spell->data.type != 0x00) //0x00 == "Spell" (ignore voice/ability/disease/etc)
			return magicSkillStrings.NullString;

		for (UInt32 i = 0; i < spell->effectItemList.count; ++i)
		{
			MagicItem::EffectItem* effect = NULL;
			spell->effectItemList.GetNthItem(i, effect);
			if (effect && effect->mgef->properties.school >= 18 && effect->mgef->properties.school <= 22)
				return magicSkillStrings.LookupSkillString(effect->mgef->properties.school);
		}
		return magicSkillStrings.NullString;
	}

	UInt32 GetSpellSkillNumber(StaticFunctionTag* base, SpellItem* spell)
	{
		if (!spell || spell->data.type != 0x00) //0x00 == "Spell" (ignore voice/ability/disease/etc)
			return 0xFFFFFFFF;

		for (UInt32 i = 0; i < spell->effectItemList.count; ++i)
		{
			MagicItem::EffectItem* effect = NULL;
			spell->effectItemList.GetNthItem(i, effect);
			if (effect)
			{
				UInt32 skillNum = effect->mgef->properties.school - 18;
				if (skillNum < 5)
					return skillNum;
			}
		}
		return 0xFFFFFFFF;
	}


	void DumpEnchantmentValues(EnchantmentItem* pEnch)
	{
		if (!pEnch) { _MESSAGE("Cant dump values on a NULL enchantment");
			return; }

		for (UInt32 i = 0; i < pEnch->effectItemList.count; i++)
		{
			MagicItem::EffectItem* pEff = NULL;
			pEnch->effectItemList.GetNthItem(i, pEff);

			//unk14 == condition

			Condition* pCond = reinterpret_cast<Condition*>(pEff->unk14);//(pEff->condition);
			if (pCond)
			{
				_MESSAGE("    Effect %u Conditions:        ", i);
				for (UInt32 count = 1; pCond; pCond = pCond->next, count++)
				{
					_MESSAGE("      Condition #%u              Function: %s", count, GetFunctionName(pCond->functionId));
				}
			}
			else
				_MESSAGE("    Condition %u:               NULL (%u) (%u)", i, pCond, pEff->unk14);//pEff->condition);
		}

		//base value
		// TESValueForm* pValue = DYNAMIC_CAST(equippedForm, TESForm, TESValueForm);
		// _MESSAGE("\n    Weapon Base Value:         %d", pValue ? pValue->value : 0);

		//enchant values
		_MESSAGE("    Manual Calc Flag Set?      %s", ((pEnch->data.calculations.flags & 0x01) == 0x01) ? "YES" : "NO");
		_MESSAGE("    'Enchantment Cost':        %d", pEnch->data.calculations.cost);
		_MESSAGE("    'Ench Amount':             %d", pEnch->data.unk0C);
		_MESSAGE("    Enchantment Auto Value:    %d\n\n", utilFuncs::CalcMagicItemCost(DYNAMIC_CAST(pEnch, EnchantmentItem, MagicItem)));

		EnchantmentItem* pBaseEnch = pEnch->data.baseEnchantment;
		_MESSAGE("    (acquired base enchantment)");
		_MESSAGE("    BASE Enchantment:          %08X", pBaseEnch ? pBaseEnch->formID : 0x00000000);

	}


	void DumpEnchantedWeaponValues(StaticFunctionTag* base)
	{

		// //TEST DUMP OF ALL ENCHANTMENTS.....
		// DataHandler* dh = DataHandler::GetSingleton();

		// for(UInt32 i = 0; i < dh->enchantments.count; i++)
		// {
		// 	EnchantmentItem* pEI = NULL;
		// 	dh->enchantments.GetNthItem(i, pEI);
		// 	TESFullName* pFN = DYNAMIC_CAST(pEI, EnchantmentItem, TESFullName);
		// 	_MESSAGE("DH->ench[%3u]  =  %s (0x%08X)", i, pFN ? pFN->name.data : "NO NAME", pEI->formID);
		// }


		_MESSAGE("DUMPING EQUIPPED WEAPON VALUE DATA...");

		PlayerCharacter* pPC = (*g_thePlayer);
		TESForm* equippedForm = pPC->GetEquippedObject(false); //false == right hand
		if (!equippedForm)
			return;

		//base name
		TESFullName* pFullName = DYNAMIC_CAST(equippedForm, TESForm, TESFullName);
		if (!pFullName)
			return;
		_MESSAGE("    Weapon Base Name:          %s", pFullName->name.data);

		//display name
		EquipData equipData = utilFuncs::ResolveEquippedObject(pPC, 1, 0);
		if(!(equipData.pForm && equipData.pExtraData))
			return;
		_MESSAGE("    Weapon Display Name:       %s", (utilFuncs::GetDisplayName(equipData.pForm, equipData.pExtraData)).data);

		//weapon enchantment
		TESObjectWEAP* pWeap = DYNAMIC_CAST(equippedForm, TESForm, TESObjectWEAP);
		EnchantmentItem* pEnch = pWeap->enchantable.enchantment;
		if (!pEnch)
		{
			pEnch = utilFuncs::GetExtraEnchantment(pPC, pWeap);
			if (!pEnch)
				_MESSAGE("    Enchantment Type:          NONE");
			else _MESSAGE("    Enchantment Type:          PLAYER-ENCHANTED");
		} else _MESSAGE("    Enchantment Type:          STANDARD");
		if (pEnch)
			_MESSAGE("    Enchantment Name:          %s", (DYNAMIC_CAST(pEnch, EnchantmentItem, TESFullName))->name.data);


		DumpEnchantmentValues(pEnch);
	}
}


template <typename T>
void UnpackValue(VMArray<T*>* dst, VMValue* src)
{
	UnpackArray(dst, src, GetTypeIDFromFormTypeID(T::kTypeID, (*g_skyrimVM)->GetClassRegistry()) | VMValue::kType_Identifier);
}

// template <> void UnpackValue(VMArray<TESForm*> * dst, VMValue * src)
// {
// 	UnpackArray(dst, src, GetTypeIDFromFormTypeID(TESForm::kTypeID, (*g_skyrimVM)->GetClassRegistry()) | VMValue::kType_Identifier);
// }

// template <> void UnpackValue(VMArray<EnchantmentItem*> * dst, VMValue *


// template <> void UnpackValue(VMArray<BGSKeyword*> * dst, VMValue * src)
// {
// 	UnpackArray(dst, src, GetTypeIDFromFormTypeID(BGSKeyword::kTypeID, (*g_skyrimVM)->GetClassRegistry()) | VMValue::kType_Identifier);
// }



bool RegisterPapyrusEAExtender(VMClassRegistry* registry)
{
	// Registration goes here, essentially is the same as the "RegisterFuncs" function used by ever other SKSE Papyrus class
	// See PapyrusSKSE.cpp for example, PLEASE DO NOT REGISTER CLASS FUNCTIONS, ONLY GLOBAL
	// Registering class functions causes conflict problems when you need to ship
	// the same pex files as SKSE would
	// Class functions are NOT faster than global functions, only more convenient

	registry->RegisterFunction(
		new NativeFunction1<StaticFunctionTag, UInt32, SpellItem*>("GetSpellSkillNumber", "EA_Extender", papyrusEAExtender::GetSpellSkillNumber, registry));
	registry->RegisterFunction(
		new NativeFunction1<StaticFunctionTag, BSFixedString, SpellItem*>("GetSpellSkillString", "EA_Extender", papyrusEAExtender::GetSpellSkillString, registry));
	registry->RegisterFunction(
		new NativeFunction2<StaticFunctionTag, bool, SpellItem*, BSFixedString>("IsSpellSkillType", "EA_Extender", papyrusEAExtender::IsSpellSkillType, registry));
	registry->RegisterFunction(
		new NativeFunction2<StaticFunctionTag, void, VMArray<TESForm*>, VMArray<BSFixedString>>("GetFormNames", "EA_Extender", papyrusEAExtender::GetFormNames, registry));
	

	registry->RegisterFunction(
		new NativeFunction2<StaticFunctionTag, bool, TESForm*, VMArray<TESForm*>>("CheckFormForEnchantment", "EA_Extender", papyrusEAExtender::CheckFormForEnchantment, registry));
	registry->RegisterFunction(
		new NativeFunction3<StaticFunctionTag, void, VMArray<TESForm*>, UInt32, VMArray<BGSKeyword*>>("GetFormArrayNthKeywords", "EA_Extender", papyrusEAExtender::GetFormArrayNthKeywords, registry));
	registry->RegisterFunction(
		new NativeFunction5<StaticFunctionTag, bool, TESObjectREFR*, VMArray<TESForm*>, VMArray<EnchantmentItem*>, bool, bool>("GetEnchantedForms", "EA_Extender", papyrusEAExtender::GetEnchantedForms, registry));
	registry->RegisterFunction(
		new NativeFunction0<StaticFunctionTag, void>("DumpEnchantedWeaponValues", "EA_Extender", papyrusEAExtender::DumpEnchantedWeaponValues, registry));

	registry->RegisterFunction(
		new NativeFunction3<StaticFunctionTag, void, TESForm*, UInt32, BGSKeyword*>("SetNthKeyword", "EA_Extender", papyrusEAExtender::SetNthKeyword, registry));
	registry->RegisterFunction(
		new NativeFunction3<StaticFunctionTag, void, VMArray<TESForm*>, UInt32, BGSKeyword*>("SetFormArrayNthKeyword", "EA_Extender", papyrusEAExtender::SetFormArrayNthKeyword, registry));
	registry->RegisterFunction(
		new NativeFunction3<StaticFunctionTag, void, VMArray<TESForm*>, UInt32, VMArray<BGSKeyword*>>("SetFormArrayNthKeywordArray", "EA_Extender", papyrusEAExtender::SetFormArrayNthKeywordArray, registry));


	registry->SetFunctionFlags("EA_Extender", "GetSpellSkillNumber", VMClassRegistry::kFunctionFlag_NoWait);
	registry->SetFunctionFlags("EA_Extender", "GetSpellSkillString", VMClassRegistry::kFunctionFlag_NoWait);
	registry->SetFunctionFlags("EA_Extender", "IsSpellSkillType", VMClassRegistry::kFunctionFlag_NoWait);
	registry->SetFunctionFlags("EA_Extender", "GetFormNames", VMClassRegistry::kFunctionFlag_NoWait);

	registry->SetFunctionFlags("EA_Extender", "CheckFormForEnchantment", VMClassRegistry::kFunctionFlag_NoWait);
	registry->SetFunctionFlags("EA_Extender", "GetFormArrayNthKeywords", VMClassRegistry::kFunctionFlag_NoWait);
	registry->SetFunctionFlags("EA_Extender", "GetEnchantedForms", VMClassRegistry::kFunctionFlag_NoWait);
	registry->SetFunctionFlags("EA_Extender", "DumpEnchantedWeaponValues", VMClassRegistry::kFunctionFlag_NoWait);

	registry->SetFunctionFlags("EA_Extender", "SetNthKeyword", VMClassRegistry::kFunctionFlag_NoWait);
	registry->SetFunctionFlags("EA_Extender", "SetFormArrayNthKeyword", VMClassRegistry::kFunctionFlag_NoWait);
	registry->SetFunctionFlags("EA_Extender", "SetFormArrayNthKeywordArray", VMClassRegistry::kFunctionFlag_NoWait);

	return true;
}


extern "C"
{

bool SKSEPlugin_Query(const SKSEInterface * skse, PluginInfo * info)
{
	gLog.OpenRelative(CSIDL_MYDOCUMENTS, kLogPath);
	_MESSAGE("EA_Extender (by egocarib)\n\nEnchanting Awakened Extender Loading...");

	// populate info structure
	info->infoVersion =	PluginInfo::kInfoVersion;
	info->name =		"EA Extender";
	info->version =		1;

	// store plugin handle so we can identify ourselves later
	g_pluginHandle = skse->GetPluginHandle();

	if(skse->isEditor)
	{
		_MESSAGE("loaded in editor, marking as incompatible");

		return false;
	}
	else if(skse->runtimeVersion != RUNTIME_VERSION_1_9_32_0)
	{
		_MESSAGE("unsupported runtime version %08X", skse->runtimeVersion);

		return false;
	}

	// get the scaleform interface and query its version
	g_scaleform = (SKSEScaleformInterface *)skse->QueryInterface(kInterface_Scaleform);
	if(!g_scaleform)
	{
		_MESSAGE("couldn't get scaleform interface");

		return false;
	}

	if(g_scaleform->interfaceVersion < SKSEScaleformInterface::kInterfaceVersion)
	{
		_MESSAGE("scaleform interface too old (%d expected %d)", g_scaleform->interfaceVersion, SKSEScaleformInterface::kInterfaceVersion);

		return false;
	}

	// get the serialization interface and query its version
	g_serialization = (SKSESerializationInterface *)skse->QueryInterface(kInterface_Serialization);
	if(!g_serialization)
	{
		_MESSAGE("couldn't get serialization interface");

		return false;
	}

	if(g_serialization->version < SKSESerializationInterface::kVersion)
	{
		_MESSAGE("serialization interface too old (%d expected %d)", g_serialization->version, SKSESerializationInterface::kVersion);

		return false;
	}

	// get the papyrus interface and query its version
	g_papyrus = (SKSEPapyrusInterface *)skse->QueryInterface(kInterface_Papyrus);
	if(!g_papyrus)
	{
		_MESSAGE("couldn't get papyrus interface");

		return false;
	}

	if(g_papyrus->interfaceVersion < SKSEPapyrusInterface::kInterfaceVersion)
	{
		_MESSAGE("papyrus interface too old (%d expected %d)", g_papyrus->interfaceVersion, SKSEPapyrusInterface::kInterfaceVersion);

		return false;
	}

	// ### do not do anything else in this callback
	// ### only fill out PluginInfo and return true/false

	// supported runtime version
	return true;
}

bool SKSEPlugin_Load(const SKSEInterface * skse)
{
	_MESSAGE("Interfacing with Papyrus...");

	g_papyrus->Register(RegisterPapyrusEAExtender);

	return true;
}

};



char* GetFunctionName(UInt32 functionID)
{
switch (functionID)
{
	case 0:  {return "GetWantBlocking";}
	case 1:  {return "GetDistance";}
	case 2:  {return "AddItem";}
	case 3:  {return "SetEssential";}
	case 4:  {return "Rotate";}
	case 5:  {return "GetLocked";}
	case 6:  {return "GetPos";}
	case 7:  {return "SetPos";}
	case 8:  {return "GetAngle";}
	case 9:  {return "SetAngle";}
	case 10:  {return "GetStartingPos";}
	case 11:  {return "GetStartingAngle";}
	case 12:  {return "GetSecondsPassed";}
	case 13:  {return "Activate";}
	case 14:  {return "GetActorValue (GetAV)";}
	case 15:  {return "SetActorValue (SetAV)";}
	case 16:  {return "ModActorValue (ModAV)";}
	case 17:  {return "SetAtStart";}
	case 18:  {return "GetCurrentTime";}
	case 19:  {return "PlayGroup";}
	case 20:  {return "LoopGroup";}
	case 21:  {return "SkipAnim";}
	case 22:  {return "StartCombat";}
	case 23:  {return "StopCombat";}
	case 24:  {return "GetScale";}
	case 25:  {return "IsMoving";}
	case 26:  {return "IsTurning";}
	case 27:  {return "GetLineOfSight (GetLOS)";}
	case 28:  {return "AddSpell";}
	case 29:  {return "RemoveSpell";}
	case 30:  {return "Cast";}
	case 31:  {return "GetButtonPressed";}
	case 32:  {return "GetInSameCell";}
	case 33:  {return "Enable";}
	case 34:  {return "Disable";}
	case 35:  {return "GetDisabled";}
	case 36:  {return "MenuMode";}
	case 37:  {return "PlaceAtMe";}
	case 38:  {return "PlaySound";}
	case 39:  {return "GetDisease";}
	case 40:  {return "FailAllObjectives";}
	case 41:  {return "GetClothingValue";}
	case 42:  {return "SameFaction";}
	case 43:  {return "SameRace";}
	case 44:  {return "SameSex";}
	case 45:  {return "GetDetected";}
	case 46:  {return "GetDead";}
	case 47:  {return "GetItemCount";}
	case 48:  {return "GetGold";}
	case 49:  {return "GetSleeping";}
	case 50:  {return "GetTalkedToPC";}
	case 51:  {return "Say";}
	case 52:  {return "SayTo";}
	case 53:  {return "GetScriptVariable";}
	case 54:  {return "StartQuest";}
	case 55:  {return "StopQuest";}
	case 56:  {return "GetQuestRunning (GetQR)";}
	case 57:  {return "SetStage";}
	case 58:  {return "GetStage";}
	case 59:  {return "GetStageDone";}
	case 60:  {return "GetFactionRankDifference";}
	case 61:  {return "GetAlarmed";}
	case 62:  {return "IsRaining";}
	case 63:  {return "GetAttacked";}
	case 64:  {return "GetIsCreature";}
	case 65:  {return "GetLockLevel";}
	case 66:  {return "GetShouldAttack";}
	case 67:  {return "GetInCell";}
	case 68:  {return "GetIsClass";}
	case 69:  {return "GetIsRace";}
	case 70:  {return "GetIsSex";}
	case 71:  {return "GetInFaction";}
	case 72:  {return "GetIsID";}
	case 73:  {return "GetFactionRank";}
	case 74:  {return "GetGlobalValue";}
	case 75:  {return "IsSnowing";}
	case 76:  {return "FastTravel (ft)";}
	case 77:  {return "GetRandomPercent";}
	case 78:  {return "RemoveMusic";}
	case 79:  {return "GetQuestVariable";}
	case 80:  {return "GetLevel";}
	case 81:  {return "IsRotating";}
	case 82:  {return "RemoveItem";}
	case 83:  {return "GetLeveledEncounterValue";}
	case 84:  {return "GetDeadCount";}
	case 85:  {return "AddToMap (ShowMap)";}
	case 86:  {return "StartConversation";}
	case 87:  {return "Drop";}
	case 88:  {return "AddTopic";}
	case 89:  {return "ShowMessage";}
	case 90:  {return "SetAlert";}
	case 91:  {return "GetIsAlerted";}
	case 92:  {return "Look";}
	case 93:  {return "StopLook";}
	case 94:  {return "EvaluatePackage (evp)";}
	case 95:  {return "SendAssaultAlarm";}
	case 96:  {return "EnablePlayerControls (epc)";}
	case 97:  {return "DisablePlayerControls (dpc)";}
	case 98:  {return "GetPlayerControlsDisabled (gpc)";}
	case 99:  {return "GetHeadingAngle";}
	case 100:  {return "PickIdle";}
	case 101:  {return "IsWeaponMagicOut";}
	case 102:  {return "IsTorchOut";}
	case 103:  {return "IsShieldOut";}
	case 104:  {return "CreateDetectionEvent";}
	case 105:  {return "IsActionRef";}
	case 106:  {return "IsFacingUp";}
	case 107:  {return "GetKnockedState";}
	case 108:  {return "GetWeaponAnimType";}
	case 109:  {return "IsWeaponSkillType";}
	case 110:  {return "GetCurrentAIPackage";}
	case 111:  {return "IsWaiting";}
	case 112:  {return "IsIdlePlaying";}
	case 113:  {return "CompleteQuest";}
	case 114:  {return "Lock";}
	case 115:  {return "UnLock";}
	case 116:  {return "IsIntimidatedbyPlayer";}
	case 117:  {return "IsPlayerInRegion";}
	case 118:  {return "GetActorAggroRadiusViolated";}
	case 119:  {return "GetCrimeKnown";}
	case 120:  {return "SetEnemy";}
	case 121:  {return "SetAlly";}
	case 122:  {return "GetCrime";}
	case 123:  {return "IsGreetingPlayer";}
	case 124:  {return "StartMisterSandMan";}
	case 125:  {return "IsGuard";}
	case 126:  {return "StartCannibal";}
	case 127:  {return "HasBeenEaten";}
	case 128:  {return "GetStaminaPercentage (GetStamina)";}
	case 129:  {return "GetPCIsClass";}
	case 130:  {return "GetPCIsRace";}
	case 131:  {return "GetPCIsSex";}
	case 132:  {return "GetPCInFaction";}
	case 133:  {return "SameFactionAsPC";}
	case 134:  {return "SameRaceAsPC";}
	case 135:  {return "SameSexAsPC";}
	case 136:  {return "GetIsReference";}
	case 137:  {return "SetFactionRank";}
	case 138:  {return "ModFactionRank";}
	case 139:  {return "KillActor (kill)";}
	case 140:  {return "ResurrectActor (resurrect)";}
	case 141:  {return "IsTalking";}
	case 142:  {return "GetWalkSpeed (GetWalk)";}
	case 143:  {return "GetCurrentAIProcedure";}
	case 144:  {return "GetTrespassWarningLevel";}
	case 145:  {return "IsTrespassing";}
	case 146:  {return "IsInMyOwnedCell";}
	case 147:  {return "GetWindSpeed";}
	case 148:  {return "GetCurrentWeatherPercent (getweatherpct)";}
	case 149:  {return "GetIsCurrentWeather (getweather)";}
	case 150:  {return "IsContinuingPackagePCNear";}
	case 151:  {return "SetCrimeFaction";}
	case 152:  {return "GetIsCrimeFaction";}
	case 153:  {return "CanHaveFlames";}
	case 154:  {return "HasFlames";}
	case 155:  {return "AddFlames";}
	case 156:  {return "RemoveFlames";}
	case 157:  {return "GetOpenState";}
	case 158:  {return "MoveToMarker (MoveTo)";}
	case 159:  {return "GetSitting";}
	case 160:  {return "GetFurnitureMarkerID";}
	case 161:  {return "GetIsCurrentPackage";}
	case 162:  {return "IsCurrentFurnitureRef";}
	case 163:  {return "IsCurrentFurnitureObj";}
	case 164:  {return "SetSize (CSize)";}
	case 165:  {return "RemoveMe";}
	case 166:  {return "DropMe";}
	case 167:  {return "GetFactionReaction";}
	case 168:  {return "SetFactionReaction";}
	case 169:  {return "ModFactionReaction";}
	case 170:  {return "GetDayOfWeek";}
	case 171:  {return "IgnoreCrime";}
	case 172:  {return "GetTalkedToPCParam";}
	case 173:  {return "RemoveAllItems";}
	case 174:  {return "WakeUpPC";}
	case 175:  {return "IsPCSleeping";}
	case 176:  {return "IsPCAMurderer";}
	case 177:  {return "SetCombatStyle (setcs)";}
	case 178:  {return "PlaySound3D";}
	case 179:  {return "SelectPlayerSpell (spspell)";}
	case 180:  {return "HasSameEditorLocAsRef";}
	case 181:  {return "HasSameEditorLocAsRefAlias";}
	case 182:  {return "GetEquipped";}
	case 183:  {return "Wait";}
	case 184:  {return "StopWaiting";}
	case 185:  {return "IsSwimming";}
	case 186:  {return "ScriptEffectElapsedSeconds";}
	case 187:  {return "SetCellPublicFlag (setpublic)";}
	case 188:  {return "GetPCSleepHours";}
	case 189:  {return "SetPCSleepHours";}
	case 190:  {return "GetAmountSoldStolen";}
	case 191:  {return "ModAmountSoldStolen";}
	case 192:  {return "GetIgnoreCrime";}
	case 193:  {return "GetPCExpelled";}
	case 194:  {return "SetPCExpelled";}
	case 195:  {return "GetPCFactionMurder";}
	case 196:  {return "SetPCFactionMurder";}
	case 197:  {return "GetPCEnemyofFaction";}
	case 198:  {return "SetPCEnemyofFaction";}
	case 199:  {return "GetPCFactionAttack";}
	case 200:  {return "SetPCFactionAttack";}
	case 201:  {return "StartScene";}
	case 202:  {return "StopScene";}
	case 203:  {return "GetDestroyed";}
	case 204:  {return "SetDestroyed";}
	case 205:  {return "GetActionRef (getAR)";}
	case 206:  {return "GetSelf (this)";}
	case 207:  {return "GetContainer";}
	case 208:  {return "GetForceRun";}
	case 209:  {return "SetForceRun";}
	case 210:  {return "GetForceSneak";}
	case 211:  {return "SetForceSneak";}
	case 212:  {return "AdvancePCSkill (AdvSkill)";}
	case 213:  {return "AdvancePCLevel (AdvLevel)";}
	case 214:  {return "HasMagicEffect";}
	case 215:  {return "GetDefaultOpen";}
	case 216:  {return "SetDefaultOpen";}
	case 217:  {return "ShowClassMenu";}
	case 218:  {return "ShowRaceMenu (SetPlayerRace)";}
	case 219:  {return "GetAnimAction";}
	case 220:  {return "ShowNameMenu";}
	case 221:  {return "SetOpenState";}
	case 222:  {return "ResetReference (RecycleActor)";}
	case 223:  {return "IsSpellTarget";}
	case 224:  {return "GetVATSMode";}
	case 225:  {return "GetPersuasionNumber";}
	case 226:  {return "GetVampireFeed";}
	case 227:  {return "GetCannibal";}
	case 228:  {return "GetIsClassDefault";}
	case 229:  {return "GetClassDefaultMatch";}
	case 230:  {return "GetInCellParam";}
	case 231:  {return "UnusedFunction1";}
	case 232:  {return "GetCombatTarget";}
	case 233:  {return "GetPackageTarget";}
	case 234:  {return "ShowSpellMaking";}
	case 235:  {return "GetVatsTargetHeight";}
	case 236:  {return "SetGhost";}
	case 237:  {return "GetIsGhost";}
	case 238:  {return "EquipItem (EquipObject)";}
	case 239:  {return "UnequipItem (UnEquipObject)";}
	case 240:  {return "SetClass";}
	case 241:  {return "SetUnconscious";}
	case 242:  {return "GetUnconscious";}
	case 243:  {return "SetRestrained";}
	case 244:  {return "GetRestrained";}
	case 245:  {return "ForceFlee (Flee)";}
	case 246:  {return "GetIsUsedItem";}
	case 247:  {return "GetIsUsedItemType";}
	case 248:  {return "IsScenePlaying";}
	case 249:  {return "IsInDialogueWithPlayer";}
	case 250:  {return "GetLocationCleared";}
	case 251:  {return "SetLocationCleared";}
	case 252:  {return "ForceRefIntoAlias";}
	case 253:  {return "EmptyRefAlias";}
	case 254:  {return "GetIsPlayableRace";}
	case 255:  {return "GetOffersServicesNow";}
	case 256:  {return "GetGameSetting (GetGS)";}
	case 257:  {return "StopCombatAlarmOnActor (SCAOnActor)";}
	case 258:  {return "HasAssociationType";}
	case 259:  {return "HasFamilyRelationship (Family)";}
	case 260:  {return "SetWeather (sw)";}
	case 261:  {return "HasParentRelationship (IsParent)";}
	case 262:  {return "IsWarningAbout";}
	case 263:  {return "IsWeaponOut";}
	case 264:  {return "HasSpell";}
	case 265:  {return "IsTimePassing";}
	case 266:  {return "IsPleasant";}
	case 267:  {return "IsCloudy";}
	case 268:  {return "TrapUpdate";}
	case 269:  {return "ShowQuestObjectives (SQO)";}
	case 270:  {return "ForceActorValue (ForceAV)";}
	case 271:  {return "IncrementPCSkill (IncPCS)";}
	case 272:  {return "DoTrap";}
	case 273:  {return "EnableFastTravel (EnableFast)";}
	case 274:  {return "IsSmallBump";}
	case 275:  {return "GetParentRef";}
	case 276:  {return "PlayBink";}
	case 277:  {return "GetBaseActorValue (GetBaseAV)";}
	case 278:  {return "IsOwner";}
	case 279:  {return "SetOwnership";}
	case 280:  {return "IsCellOwner";}
	case 281:  {return "SetCellOwnership";}
	case 282:  {return "IsHorseStolen";}
	case 283:  {return "SetCellFullName";}
	case 284:  {return "SetActorFullName";}
	case 285:  {return "IsLeftUp";}
	case 286:  {return "IsSneaking";}
	case 287:  {return "IsRunning";}
	case 288:  {return "GetFriendHit";}
	case 289:  {return "IsInCombat";}
	case 290:  {return "SetPackDuration (SPDur)";}
	case 291:  {return "PlayMagicShaderVisuals (PMS)";}
	case 292:  {return "PlayMagicEffectVisuals (PME)";}
	case 293:  {return "StopMagicShaderVisuals (SMS)";}
	case 294:  {return "StopMagicEffectVisuals (SME)";}
	case 295:  {return "ResetInterior";}
	case 296:  {return "IsAnimPlaying";}
	case 297:  {return "SetActorAlpha (SAA)";}
	case 298:  {return "EnableLinkedPathPoints";}
	case 299:  {return "DisableLinkedPathPoints";}
	case 300:  {return "IsInInterior";}
	case 301:  {return "ForceWeather (fw)";}
	case 302:  {return "ToggleActorsAI";}
	case 303:  {return "IsActorsAIOff";}
	case 304:  {return "IsWaterObject";}
	case 305:  {return "GetPlayerAction";}
	case 306:  {return "IsActorUsingATorch";}
	case 307:  {return "SetLevel";}
	case 308:  {return "ResetFallDamageTimer";}
	case 309:  {return "IsXBox";}
	case 310:  {return "GetInWorldspace";}
	case 311:  {return "ModPCMiscStat (ModPCMS)";}
	case 312:  {return "GetPCMiscStat (GetPCMS)";}
	case 313:  {return "GetPairedAnimation (GPA)";}
	case 314:  {return "IsActorAVictim";}
	case 315:  {return "GetTotalPersuasionNumber";}
	case 316:  {return "SetScale";}
	case 317:  {return "ModScale";}
	case 318:  {return "GetIdleDoneOnce";}
	case 319:  {return "KillAllActors (killall)";}
	case 320:  {return "GetNoRumors";}
	case 321:  {return "SetNoRumors";}
	case 322:  {return "Dispel";}
	case 323:  {return "GetCombatState";}
	case 324:  {return "TriggerHitShader (ths)";}
	case 325:  {return "GetWithinPackageLocation";}
	case 326:  {return "Reset3DState";}
	case 327:  {return "IsRidingHorse";}
	case 328:  {return "DispelAllSpells";}
	case 329:  {return "IsFleeing";}
	case 330:  {return "AddAchievement";}
	case 331:  {return "DuplicateAllItems";}
	case 332:  {return "IsInDangerousWater";}
	case 333:  {return "EssentialDeathReload";}
	case 334:  {return "SetShowQuestItems";}
	case 335:  {return "DuplicateNPCStats";}
	case 336:  {return "ResetHealth";}
	case 337:  {return "SetIgnoreFriendlyHits (sifh)";}
	case 338:  {return "GetIgnoreFriendlyHits (gifh)";}
	case 339:  {return "IsPlayersLastRiddenHorse";}
	case 340:  {return "SetActorRefraction (sar)";}
	case 341:  {return "SetItemValue";}
	case 342:  {return "SetRigidBodyMass";}
	case 343:  {return "ShowViewerStrings (svs)";}
	case 344:  {return "ReleaseWeatherOverride (rwo)";}
	case 345:  {return "SetAllReachable";}
	case 346:  {return "SetAllVisible";}
	case 347:  {return "SetNoAvoidance";}
	case 348:  {return "SendTrespassAlarm";}
	case 349:  {return "SetSceneIsComplex";}
	case 350:  {return "Autosave";}
	case 351:  {return "StartMasterFileSeekData";}
	case 352:  {return "DumpMasterFileSeekData";}
	case 353:  {return "IsActor";}
	case 354:  {return "IsEssential";}
	case 355:  {return "PreloadMagicEffect";}
	case 356:  {return "ShowDialogSubtitles";}
	case 357:  {return "SetPlayerResistingArrest";}
	case 358:  {return "IsPlayerMovingIntoNewSpace";}
	case 359:  {return "GetInCurrentLoc";}
	case 360:  {return "GetInCurrentLocAlias";}
	case 361:  {return "GetTimeDead";}
	case 362:  {return "HasLinkedRef";}
	case 363:  {return "GetLinkedRef";}
	case 364:  {return "DamageObject (do)";}
	case 365:  {return "IsChild";}
	case 366:  {return "GetStolenItemValueNoCrime";}
	case 367:  {return "GetLastPlayerAction";}
	case 368:  {return "IsPlayerActionActive";}
	case 369:  {return "SetTalkingActivatorActor";}
	case 370:  {return "IsTalkingActivatorActor";}
	case 371:  {return "ShowBarterMenu (sbm)";}
	case 372:  {return "IsInList";}
	case 373:  {return "GetStolenItemValue";}
	case 374:  {return "AddPerk";}
	case 375:  {return "GetCrimeGoldViolent (getviolent)";}
	case 376:  {return "GetCrimeGoldNonviolent (getnonviolent)";}
	case 377:  {return "ShowRepairMenu (srm)";}
	case 378:  {return "HasShout";}
	case 379:  {return "AddNote (AN)";}
	case 380:  {return "RemoveNote (RN)";}
	case 381:  {return "GetHasNote (GetN)";}
	case 382:  {return "AddToFaction (Addfac)";}
	case 383:  {return "RemoveFromFaction (Removefac)";}
	case 384:  {return "DamageActorValue (DamageAV)";}
	case 385:  {return "RestoreActorValue (RestoreAV)";}
	case 386:  {return "TriggerHUDShudder (hudsh)";}
	case 387:  {return "GetObjectiveFailed";}
	case 388:  {return "SetObjectiveFailed";}
	case 389:  {return "SetGlobalTimeMultiplier (sgtm)";}
	case 390:  {return "GetHitLocation";}
	case 391:  {return "IsPC1stPerson (pc1st)";}
	case 392:  {return "PurgeCellBuffers (pcb)";}
	case 393:  {return "PushActorAway";}
	case 394:  {return "SetActorsAI";}
	case 395:  {return "ClearOwnership";}
	case 396:  {return "GetCauseofDeath";}
	case 397:  {return "IsLimbGone";}
	case 398:  {return "IsWeaponInList";}
	case 399:  {return "PlayIdle";}
	case 400:  {return "ApplyImageSpaceModifier (imod)";}
	case 401:  {return "RemoveImageSpaceModifier (rimod)";}
	case 402:  {return "IsBribedbyPlayer";}
	case 403:  {return "GetRelationshipRank";}
	case 404:  {return "SetRelationshipRank";}
	case 405:  {return "SetCellImageSpace";}
	case 406:  {return "ShowChargenMenu (scgm)";}
	case 407:  {return "GetVATSValue";}
	case 408:  {return "IsKiller";}
	case 409:  {return "IsKillerObject";}
	case 410:  {return "GetFactionCombatReaction";}
	case 411:  {return "UseWeapon";}
	case 412:  {return "EvaluateSpellConditions (esc)";}
	case 413:  {return "ToggleMotionBlur (tmb)";}
	case 414:  {return "Exists";}
	case 415:  {return "GetGroupMemberCount";}
	case 416:  {return "GetGroupTargetCount";}
	case 417:  {return "SetObjectiveCompleted";}
	case 418:  {return "SetObjectiveDisplayed";}
	case 419:  {return "GetObjectiveCompleted";}
	case 420:  {return "GetObjectiveDisplayed";}
	case 421:  {return "SetImageSpace";}
	case 422:  {return "PipboyRadio (prad)";}
	case 423:  {return "RemovePerk";}
	case 424:  {return "DisableAllActors (DisAA)";}
	case 425:  {return "GetIsFormType";}
	case 426:  {return "GetIsVoiceType";}
	case 427:  {return "GetPlantedExplosive";}
	case 428:  {return "CompleteAllObjectives";}
	case 429:  {return "IsScenePackageRunning";}
	case 430:  {return "GetHealthPercentage";}
	case 431:  {return "SetAudioMultithreading (SAM)";}
	case 432:  {return "GetIsObjectType";}
	case 433:  {return "ShowChargenMenuParams (scgmp)";}
	case 434:  {return "GetDialogueEmotion";}
	case 435:  {return "GetDialogueEmotionValue";}
	case 436:  {return "ExitGame (exit)";}
	case 437:  {return "GetIsCreatureType";}
	case 438:  {return "PlayerCreatePotion";}
	case 439:  {return "PlayerEnchantObject";}
	case 440:  {return "ShowWarning";}
	case 441:  {return "EnterTrigger";}
	case 442:  {return "MarkForDelete";}
	case 443:  {return "SetPlayerAIDriven";}
	case 444:  {return "GetInCurrentLocFormList";}
	case 445:  {return "GetInZone";}
	case 446:  {return "GetVelocity";}
	case 447:  {return "GetGraphVariableFloat";}
	case 448:  {return "HasPerk";}
	case 449:  {return "GetFactionRelation";}
	case 450:  {return "IsLastIdlePlayed";}
	case 451:  {return "SetNPCRadio (snr)";}
	case 452:  {return "SetPlayerTeammate";}
	case 453:  {return "GetPlayerTeammate";}
	case 454:  {return "GetPlayerTeammateCount";}
	case 455:  {return "OpenActorContainer";}
	case 456:  {return "ClearFactionPlayerEnemyFlag";}
	case 457:  {return "ClearActorsFactionsPlayerEnemyFlag";}
	case 458:  {return "GetActorCrimePlayerEnemy";}
	case 459:  {return "GetCrimeGold";}
	case 460:  {return "SetCrimeGold";}
	case 461:  {return "ModCrimeGold";}
	case 462:  {return "GetPlayerGrabbedRef";}
	case 463:  {return "IsPlayerGrabbedRef";}
	case 464:  {return "PlaceLeveledActorAtMe";}
	case 465:  {return "GetKeywordItemCount";}
	case 466:  {return "ShowLockpickMenu (slpm)";}
	case 467:  {return "GetBroadcastState";}
	case 468:  {return "SetBroadcastState";}
	case 469:  {return "StartRadioConversation";}
	case 470:  {return "GetDestructionStage";}
	case 471:  {return "ClearDestruction";}
	case 472:  {return "CastImmediateOnSelf (cios)";}
	case 473:  {return "GetIsAlignment";}
	case 474:  {return "ResetQuest";}
	case 475:  {return "SetQuestDelay";}
	case 476:  {return "IsProtected";}
	case 477:  {return "GetThreatRatio";}
	case 478:  {return "MatchFaceGeometry";}
	case 479:  {return "GetIsUsedItemEquipType";}
	case 480:  {return "GetPlayerName";}
	case 481:  {return "FireWeapon";}
	case 482:  {return "PayCrimeGold";}
	case 483:  {return "UnusedFunction2";}
	case 484:  {return "MatchRace";}
	case 485:  {return "SetPCYoung";}
	case 486:  {return "SexChange";}
	case 487:  {return "IsCarryable";}
	case 488:  {return "GetConcussed";}
	case 489:  {return "SetZoneRespawns";}
	case 490:  {return "SetVATSTarget";}
	case 491:  {return "GetMapMarkerVisible";}
	case 492:  {return "ResetInventory";}
	case 493:  {return "PlayerKnows";}
	case 494:  {return "GetPermanentActorValue (GetPermAV)";}
	case 495:  {return "GetKillingBlowLimb";}
	case 496:  {return "GoToJail";}
	case 497:  {return "CanPayCrimeGold";}
	case 498:  {return "ServeTime";}
	case 499:  {return "GetDaysInJail";}
	case 500:  {return "EPAlchemyGetMakingPoison";}
	case 501:  {return "EPAlchemyEffectHasKeyword";}
	case 502:  {return "ShowAllMapMarkers (tmm)";}
	case 503:  {return "GetAllowWorldInteractions";}
	case 504:  {return "ResetAI";}
	case 505:  {return "SetRumble";}
	case 506:  {return "SetNoActivationSound";}
	case 507:  {return "ClearNoActivationSound";}
	case 508:  {return "GetLastHitCritical";}
	case 509:  {return "AddMusic";}
	case 510:  {return "UnusedFunction3";}
	case 511:  {return "UnusedFunction4";}
	case 512:  {return "SetPCToddler";}
	case 513:  {return "IsCombatTarget";}
	case 514:  {return "TriggerScreenBlood (tsb)";}
	case 515:  {return "GetVATSRightAreaFree";}
	case 516:  {return "GetVATSLeftAreaFree";}
	case 517:  {return "GetVATSBackAreaFree";}
	case 518:  {return "GetVATSFrontAreaFree";}
	case 519:  {return "GetIsLockBroken";}
	case 520:  {return "IsPS3";}
	case 521:  {return "IsWin32";}
	case 522:  {return "GetVATSRightTargetVisible";}
	case 523:  {return "GetVATSLeftTargetVisible";}
	case 524:  {return "GetVATSBackTargetVisible";}
	case 525:  {return "GetVATSFrontTargetVisible";}
	case 526:  {return "AttachAshPile";}
	case 527:  {return "SetCriticalStage";}
	case 528:  {return "IsInCriticalStage";}
	case 529:  {return "RemoveFromAllFactions";}
	case 530:  {return "GetXPForNextLevel";}
	case 531:  {return "ShowLockpickMenuDebug (slpmd)";}
	case 532:  {return "ForceSave";}
	case 533:  {return "GetInfamy";}
	case 534:  {return "GetInfamyViolent";}
	case 535:  {return "GetInfamyNonViolent";}
	case 536:  {return "UnusedFunction5";}
	case 537:  {return "Sin";}
	case 538:  {return "Cos";}
	case 539:  {return "Tan";}
	case 540:  {return "Sqrt";}
	case 541:  {return "Log";}
	case 542:  {return "Abs";}
	case 543:  {return "GetQuestCompleted (GetQC)";}
	case 544:  {return "UnusedFunction6";}
	case 545:  {return "PipBoyRadioOff";}
	case 546:  {return "AutoDisplayObjectives";}
	case 547:  {return "IsGoreDisabled";}
	case 548:  {return "FadeSFX (FSFX)";}
	case 549:  {return "SetMinimalUse";}
	case 550:  {return "IsSceneActionComplete";}
	case 551:  {return "ShowQuestStages (SQS)";}
	case 552:  {return "GetSpellUsageNum";}
	case 553:  {return "ForceRadioStationUpdate (FRSU)";}
	case 554:  {return "GetActorsInHigh";}
	case 555:  {return "HasLoaded3D";}
	case 556:  {return "DisableAllMines";}
	case 557:  {return "SetLastExtDoorActivated";}
	case 558:  {return "KillQuestUpdates (KQU)";}
	case 559:  {return "IsImageSpaceActive";}
	case 560:  {return "HasKeyword";}
	case 561:  {return "HasRefType";}
	case 562:  {return "LocationHasKeyword";}
	case 563:  {return "LocationHasRefType";}
	case 564:  {return "CreateEvent";}
	case 565:  {return "GetIsEditorLocation";}
	case 566:  {return "GetIsAliasRef";}
	case 567:  {return "GetIsEditorLocAlias";}
	case 568:  {return "IsSprinting";}
	case 569:  {return "IsBlocking";}
	case 570:  {return "HasEquippedSpell (hasspell)";}
	case 571:  {return "GetCurrentCastingType (getcasting)";}
	case 572:  {return "GetCurrentDeliveryType (getdelivery)";}
	case 573:  {return "EquipSpell";}
	case 574:  {return "GetAttackState";}
	case 575:  {return "GetAliasedRef";}
	case 576:  {return "GetEventData";}
	case 577:  {return "IsCloserToAThanB";}
	case 578:  {return "EquipShout";}
	case 579:  {return "GetEquippedShout";}
	case 580:  {return "IsBleedingOut";}
	case 581:  {return "UnlockWord";}
	case 582:  {return "TeachWord";}
	case 583:  {return "AddToContainer";}
	case 584:  {return "GetRelativeAngle";}
	case 585:  {return "SendAnimEvent (sae)";}
	case 586:  {return "Shout";}
	case 587:  {return "AddShout";}
	case 588:  {return "RemoveShout";}
	case 589:  {return "GetMovementDirection";}
	case 590:  {return "IsInScene";}
	case 591:  {return "GetRefTypeDeadCount";}
	case 592:  {return "GetRefTypeAliveCount";}
	case 593:  {return "ApplyHavokImpulse";}
	case 594:  {return "GetIsFlying";}
	case 595:  {return "IsCurrentSpell";}
	case 596:  {return "SpellHasKeyword";}
	case 597:  {return "GetEquippedItemType";}
	case 598:  {return "GetLocationAliasCleared";}
	case 599:  {return "SetLocationAliasCleared";}
	case 600:  {return "GetLocAliasRefTypeDeadCount";}
	case 601:  {return "GetLocAliasRefTypeAliveCount";}
	case 602:  {return "IsWardState";}
	case 603:  {return "IsInSameCurrentLocAsRef";}
	case 604:  {return "IsInSameCurrentLocAsRefAlias";}
	case 605:  {return "LocAliasIsLocation";}
	case 606:  {return "GetKeywordDataForLocation";}
	case 607:  {return "SetKeywordDataForLocation";}
	case 608:  {return "GetKeywordDataForAlias";}
	case 609:  {return "SetKeywordDataForAlias";}
	case 610:  {return "LocAliasHasKeyword";}
	case 611:  {return "IsNullPackageData";}
	case 612:  {return "GetNumericPackageData";}
	case 613:  {return "IsFurnitureAnimType";}
	case 614:  {return "IsFurnitureEntryType";}
	case 615:  {return "GetHighestRelationshipRank";}
	case 616:  {return "GetLowestRelationshipRank";}
	case 617:  {return "HasAssociationTypeAny";}
	case 618:  {return "HasFamilyRelationshipAny";}
	case 619:  {return "GetPathingTargetOffset";}
	case 620:  {return "GetPathingTargetAngleOffset";}
	case 621:  {return "GetPathingTargetSpeed";}
	case 622:  {return "GetPathingTargetSpeedAngle";}
	case 623:  {return "GetMovementSpeed";}
	case 624:  {return "GetInContainer";}
	case 625:  {return "IsLocationLoaded";}
	case 626:  {return "IsLocAliasLoaded";}
	case 627:  {return "IsDualCasting";}
	case 628:  {return "DualCast";}
	case 629:  {return "GetVMQuestVariable";}
	case 630:  {return "GetVMScriptVariable";}
	case 631:  {return "IsEnteringInteractionQuick";}
	case 632:  {return "IsCasting";}
	case 633:  {return "GetFlyingState";}
	case 634:  {return "SetFavorState";}
	case 635:  {return "IsInFavorState";}
	case 636:  {return "HasTwoHandedWeaponEquipped";}
	case 637:  {return "IsExitingInstant";}
	case 638:  {return "IsInFriendStatewithPlayer";}
	case 639:  {return "GetWithinDistance";}
	case 640:  {return "GetActorValuePercent";}
	case 641:  {return "IsUnique";}
	case 642:  {return "GetLastBumpDirection";}
	case 643:  {return "CameraShake";}
	case 644:  {return "IsInFurnitureState";}
	case 645:  {return "GetIsInjured";}
	case 646:  {return "GetIsCrashLandRequest";}
	case 647:  {return "GetIsHastyLandRequest";}
	case 648:  {return "UpdateQuestInstanceGlobal";}
	case 649:  {return "SetAllowFlying";}
	case 650:  {return "IsLinkedTo";}
	case 651:  {return "GetKeywordDataForCurrentLocation";}
	case 652:  {return "GetInSharedCrimeFaction";}
	case 653:  {return "GetBribeAmount";}
	case 654:  {return "GetBribeSuccess";}
	case 655:  {return "GetIntimidateSuccess";}
	case 656:  {return "GetArrestedState";}
	case 657:  {return "GetArrestingActor";}
	case 658:  {return "ClearArrestState";}
	case 659:  {return "EPTemperingItemIsEnchanted";}
	case 660:  {return "EPTemperingItemHasKeyword";}
	case 661:  {return "GetReceivedGiftValue";}
	case 662:  {return "GetGiftGivenValue";}
	case 663:  {return "ForceLocIntoAlias";}
	case 664:  {return "GetReplacedItemType";}
	case 665:  {return "SetHorseActor";}
	case 666:  {return "PlayReferenceEffect (pre)";}
	case 667:  {return "StopReferenceEffect (sre)";}
	case 668:  {return "PlayShaderParticleGeometry (pspg)";}
	case 669:  {return "StopShaderParticleGeometry (sspg)";}
	case 670:  {return "ApplyImageSpaceModifierCrossFade (imodcf)";}
	case 671:  {return "RemoveImageSpaceModifierCrossFade (rimodcf)";}
	case 672:  {return "IsAttacking";}
	case 673:  {return "IsPowerAttacking";}
	case 674:  {return "IsLastHostileActor";}
	case 675:  {return "GetGraphVariableInt";}
	case 676:  {return "GetCurrentShoutVariation";}
	case 677:  {return "PlayImpactEffect (pie)";}
	case 678:  {return "ShouldAttackKill";}
	case 679:  {return "SendStealAlarm (steal)";}
	case 680:  {return "GetActivationHeight";}
	case 681:  {return "EPModSkillUsage_IsAdvanceSkill";}
	case 682:  {return "WornHasKeyword";}
	case 683:  {return "GetPathingCurrentSpeed";}
	case 684:  {return "GetPathingCurrentSpeedAngle";}
	case 685:  {return "KnockAreaEffect (kae)";}
	case 686:  {return "InterruptCast";}
	case 687:  {return "AddFormToFormList";}
	case 688:  {return "RevertFormList";}
	case 689:  {return "AddFormToLeveledList";}
	case 690:  {return "RevertLeveledList";}
	case 691:  {return "EPModSkillUsage_AdvanceObjectHasKeyword";}
	case 692:  {return "EPModSkillUsage_IsAdvanceAction";}
	case 693:  {return "EPMagic_SpellHasKeyword";}
	case 694:  {return "GetNoBleedoutRecovery";}
	case 695:  {return "SetNoBleedoutRecovery";}
	case 696:  {return "EPMagic_SpellHasSkill";}
	case 697:  {return "IsAttackType";}
	case 698:  {return "IsAllowedToFly";}
	case 699:  {return "HasMagicEffectKeyword";}
	case 700:  {return "IsCommandedActor";}
	case 701:  {return "IsStaggered";}
	case 702:  {return "IsRecoiling";}
	case 703:  {return "IsExitingInteractionQuick";}
	case 704:  {return "IsPathing";}
	case 705:  {return "GetShouldHelp";}
	case 706:  {return "HasBoundWeaponEquipped";}
	case 707:  {return "GetCombatTargetHasKeyword (gcthk)";}
	case 708:  {return "UpdateLevel";}
	case 709:  {return "GetCombatGroupMemberCount (gcgmc)";}
	case 710:  {return "IsIgnoringCombat";}
	case 711:  {return "GetLightLevel (gll)";}
	case 712:  {return "SavePCFace (spf)";}
	case 713:  {return "SpellHasCastingPerk";}
	case 714:  {return "IsBeingRidden";}
	case 715:  {return "IsUndead";}
	case 716:  {return "GetRealHoursPassed";}
	case 717:  {return "UnequipAll";}
	case 718:  {return "IsUnlockedDoor";}
	case 719:  {return "IsHostileToActor";}
	case 720:  {return "GetTargetHeight";}
	case 721:  {return "IsPoison";}
	case 722:  {return "WornApparelHasKeywordCount";}
	case 723:  {return "GetItemHealthPercent";}
	case 724:  {return "EffectWasDualCast";}
	case 725:  {return "GetKnockStateEnum";}
	case 726:  {return "DoesNotExist";}
	default:   {return "";}
}
}