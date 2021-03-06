#include "VbDecompiler.h"
#include <allins.hpp>
#include <struct.hpp>
#include <bytes.hpp>
#include <diskio.hpp>
#include <hexrays.hpp>
#include "ImportsFix.h"

void 设置函数名称(ea_t FuncAddr, const char* FuncName)
{
	qstring OFuncName;
	get_func_name(&OFuncName, FuncAddr);

	if (OFuncName.find("sub_") != qstring::npos)
	{
		set_name(FuncAddr, FuncName);
	}
}

//IDA存在函数识别错误的情况,需要进行刷新
void RefreshFuncion(ea_t StartFuncAddr)
{
	func_t* pfunc = get_func(StartFuncAddr);
	if (!pfunc)
	{
		return;
	}
	hexrays_failure_t hf;
	cfunc_t* cfunc = decompile_func(pfunc, &hf);
	if (!cfunc)
	{
		return;
	}
	open_pseudocode(StartFuncAddr, -1);
}

qstring FindCheckObjGUID(ea_t CheckObjAddr, ea_t& GuidAddr)
{
	qstring ret;
	unsigned int pushCount = 0;
	for (unsigned int n = 0; n < 12; ++n)
	{
		insn_t ins_Push;
		CheckObjAddr = decode_prev_insn(&ins_Push, CheckObjAddr);
		if (CheckObjAddr == BADADDR)
		{
			return ret;
		}
		//向上找第三条push指令
		if (ins_Push.itype == NN_push && ++pushCount == 3)
		{
			if (ins_Push.ops[0].type == o_imm)
			{
				GuidAddr = ins_Push.ops[0].value;
				ret = get_uuid(GuidAddr);
				break;
			}
		}
	}
	return ret;
}

bool AddFunctionComment(cfunc_t* cfunc, cexpr_t* pCheckObjCall, ObjData& eObjData)
{
	strvec_t& sv = cfunc->sv;

	carg_t& pNumVar = pCheckObjCall->a->at(3);
	uint32 VOffset = pNumVar.n->_value;

	ea_t CheckObjCALLAddr = eObjData.GuidAddr;
	unsigned int TryCount = 0;
	while (true)
	{
		insn_t tmpIns;

		CheckObjCALLAddr = decode_prev_insn(&tmpIns, CheckObjCALLAddr);
		if (CheckObjCALLAddr == BADADDR)
		{
			return false;
		}
		if (TryCount++ > 12)
		{
			return false;
		}
		if (tmpIns.itype == NN_jge)
		{
			break;
		}
	}

	TryCount = 0;
	citem_t* pComCallItem;
	while (true)
	{
		insn_t tmpIns;
		CheckObjCALLAddr = decode_prev_insn(&tmpIns, CheckObjCALLAddr);
		if (CheckObjCALLAddr == BADADDR)
		{
			return false;
		}
		if (TryCount++ > 6)
		{
			return false;
		}

		if (tmpIns.itype == NN_callni)
		{
			pComCallItem = cfunc->body.find_closest_addr(CheckObjCALLAddr);
			break;
		}
	}

	int px, py;
	if (!cfunc->find_item_coords(pComCallItem, &px, &py))
	{
		return false;
	}

	ctree_item_t treeCommentItem;
	cfunc->get_line_item(sv[py].line.c_str(), px, true, NULL, NULL, &treeCommentItem);

	qstring VTableName = eObjData.StructName + "_vtbl";
	import_type(idati, -1, VTableName.c_str());
	tid_t idStruct = get_struc_id(VTableName.c_str());
	tid_t idMember = get_member_id(get_struc(idStruct), VOffset);
	qstring CallFuncName = get_member_name(idMember);

	cfunc->set_user_cmt(treeCommentItem.loc, CallFuncName.c_str());
	return true;
}

void VBDecompilerEngine::GetAllGuidXref(std::map<ea_t, qvector<ObjData>>& outMap, ea_t HresultCheckObj)
{
	//获取所有的数据引用和代码引用
	qvector<ea_t> vec_Xref;
	ea_t XrefAddr = get_first_dref_to(HresultCheckObj);
	while (XrefAddr != BADADDR)
	{
		vec_Xref.push_back(XrefAddr);
		XrefAddr = get_next_dref_to(HresultCheckObj, XrefAddr);
	}

	XrefAddr = get_first_cref_to(HresultCheckObj);
	while (XrefAddr != BADADDR)
	{
		vec_Xref.push_back(XrefAddr);
		XrefAddr = get_next_cref_to(HresultCheckObj, XrefAddr);
	}

	//确定代码范围
	segment_t* pSegText = get_segm_by_name(".text");
	ea_t SearchStartAddr;
	ea_t SearchEndAddr;
	if (!pSegText)
	{
		SearchStartAddr = 0x401000;
		SearchEndAddr = 0x600000;
	}
	else
	{
		SearchStartAddr = pSegText->start_ea;
		SearchEndAddr = pSegText->end_ea;
	}

	//遍历所有的_vbaHresultCheckObj交叉引用数据
	for (unsigned int n = 0; n < vec_Xref.size(); ++n)
	{
		insn_t ins_Call;
		decode_insn(&ins_Call, vec_Xref[n]);
		if (ins_Call.itype != NN_callni)
		{
			continue;
		}

		//FF 15或者call register
		if (ins_Call.ops[0].type != o_mem && ins_Call.ops[0].type != o_reg)
		{
			msg("UnHandle Instruction Type:%a\n", vec_Xref[n]);
			continue;
		}

		ea_t GlobalGuidAddr = BADADDR;

		//根据vbaHresultCheckObj函数向上找到Guid
		qstring InterfaceGuid = FindCheckObjGUID(vec_Xref[n], GlobalGuidAddr);
		if (!m_GuidMap[InterfaceGuid].size())
		{
			//msg("UnHandled ObjUUID:%s\n", InterfaceGuid.c_str());
			continue;
		}

		func_t* pFunc = get_func(vec_Xref[n]);
		if (!pFunc)
		{
			continue;
		}

		ObjData data;
		qvector<coClassInfo>& vec_coClass = m_GuidMap[InterfaceGuid];
		data.GuidAddr = vec_Xref[n];
		data.StructName = vec_coClass[0].StructName;

		//同一个InterfaceGuid对应一个以上的CoClassGuid
		if (vec_coClass.size() != 1)
		{
			//To do...需要通过vbaNew和vbaNew2来进行额外的判断
			msg("Found a Special ComInterface %s,Ignored...\n", InterfaceGuid.c_str());
		}

		qstring GuidName = "GUID_";
		GuidName += data.StructName;
		set_name(GlobalGuidAddr, GuidName.c_str(), SN_NOWARN);
		outMap[pFunc->start_ea].push_back(data);
	}
	return;
}



bool VBDecompilerEngine::HandleCheckObj(func_t* pfunc, qvector<ObjData> vec_ObjData)
{
	RefreshFuncion(pfunc->start_ea);

	hexrays_failure_t hf;
	cfunc_t* cfunc = decompile_func(pfunc, &hf);
	if (!cfunc)
	{
		return false;
	}

	lvars_t* varList = cfunc->get_lvars();
	qvector<simpleline_t>& sv = cfunc->sv;

	ea_t CurrentFuncAddr = pfunc->start_ea;

	lvar_saved_infos_t UserFlashInfo;
	for (unsigned int n = 0; n < vec_ObjData.size(); ++n)
	{
		citem_t* pItem = cfunc->body.find_closest_addr(vec_ObjData[n].GuidAddr);
		if (!pItem || pItem->op != cot_call)
		{
			//这里是反编译不了的函数,跳过
			continue;
		}

		cexpr_t* pCall = (cexpr_t*)pItem;
		if (pCall->a->size() != 4)
		{
			//这里大部分还是一些异常函数,跳过
			continue;
		}

		carg_t& pObjVar = pCall->a->at(1);
		carg_t& pNumVar = pCall->a->at(3);

		if (pNumVar.op == cot_num)
		{
			//大多数情况下我们无法得知var之前的指针类型,故只能采用添加注释法
			if (!AddFunctionComment(cfunc, pCall, vec_ObjData[n]))
			{
				msg("[AddFunctionComment]:Failed...%a\n", vec_ObjData[n].GuidAddr);
			}
		}

		if (pObjVar.op == cot_obj)
		{
			ea_t Addr = vec_ObjData[n].GuidAddr;
			qstring ObjType = vec_ObjData[n].StructName + "*;";
			apply_cdecl(idati, pObjVar.obj_ea, ObjType.c_str(), TINFO_DEFINITE);
		}
		if (pObjVar.op == cot_var)
		{
			lvar_t& var = varList->at(pObjVar.v.idx);
			lvar_saved_info_t saveInfo;
			tinfo_t ObjType = create_typedef(vec_ObjData[n].StructName.c_str());
			saveInfo.ll = var;
			saveInfo.type = make_pointer(ObjType);
			UserFlashInfo.push_back(saveInfo);
		}
	}

	for (unsigned int n = 0; n < UserFlashInfo.size(); ++n)
	{
		modify_user_lvar_info(CurrentFuncAddr, MLI_TYPE, UserFlashInfo[n]);
	}

	cfunc->save_user_cmts();

	return true;
}

//初始化类结构体,返回该结构体的id,如果为-1则失败
void InitClassStructure(const qstring ClassName, qvector<qstring>& vec_MethodName, uint EventCount, uint ClassSize)
{
	qstring VTableName = ClassName + "_vtbl";
	//添加虚表
	tid_t VTableTid = get_struc_id(VTableName.c_str());
	if (VTableTid == BADADDR)
	{
		VTableTid = add_struc(BADADDR, VTableName.c_str(), false);
	}

	opinfo_t opInfo;
	opInfo.tid = get_struc_id("IDispatchVtbl");
	struc_t* pStruct = get_struc(VTableTid);

	unsigned int IDispatchOff = get_struc_size(opInfo.tid);
	add_struc_member(pStruct, "baseclass_0", 0, stru_flag(), &opInfo, IDispatchOff);
	for (unsigned int n = 0; n < EventCount; ++n)
	{
		qstring eEventName;
		eEventName.sprnt("op_%a", IDispatchOff);
		add_struc_member(pStruct, eEventName.c_str(), IDispatchOff, dword_flag(), NULL, 4);
		IDispatchOff += 4;
	}
	for (unsigned int n = 0; n < vec_MethodName.size(); ++n)
	{
		add_struc_member(pStruct, vec_MethodName[n].c_str(), IDispatchOff, dword_flag(), NULL, 4);
		IDispatchOff += 4;
	}

	//添加类结构体
	tid_t ClassTid = get_struc_id(ClassName.c_str());
	if (ClassTid == BADADDR)
	{
		ClassTid = add_struc(BADADDR, ClassName.c_str(), false);
	}

	pStruct = get_struc(ClassTid);
	add_struc_member(pStruct, "vtbl", 0, dword_flag(), &opInfo, 4);
	tinfo_t VType = make_pointer(create_typedef(VTableName.c_str()));
	set_member_tinfo(pStruct, get_member(pStruct, 0), 0, VType, SET_MEMTI_USERTI);

	for (unsigned int n = 4; n < ClassSize; ++n)
	{
		char fieldName[255] = {};
		qsnprintf(fieldName, sizeof(fieldName), "field_%a", n);
		add_struc_member(pStruct, fieldName, n, byte_flag(), NULL, 1);
	}

	return;
}

//qstring VBDecompilerEngine::GetControlTypeNameByUUID(qstring uuid)
//{
//	static std::map<qstring, qstring> map_ControlUUID;
//
//	if (!map_ControlUUID.size())
//	{
//		map_ControlUUID["33AD4ED2-6699-11CF-B70C-00AA0060D393"] = "PictureBoxEvents";
//		map_ControlUUID["33AD4EDA-6699-11CF-B70C-00AA0060D393"] = "LabelEvents";
//		map_ControlUUID["33AD4EE2-6699-11CF-B70C-00AA0060D393"] = "TextBoxEvents";
//		map_ControlUUID["33AD4EEA-6699-11CF-B70C-00AA0060D393"] = "FrameEvents";
//		map_ControlUUID["33AD4EF2-6699-11CF-B70C-00AA0060D393"] = "CommandButtonEvents";
//		map_ControlUUID["33AD4EFA-6699-11CF-B70C-00AA0060D393"] = "CheckBoxEvents";
//		map_ControlUUID["33AD4F3A-6699-11CF-B70C-00AA0060D393"] = "FormEvents";
//	}
//
//	qstring ret = map_ControlUUID[uuid];
//	if (!ret.size())
//	{
//		ret = "UserControl";
//	}
//
//	return ret;
//}

qstring VBDecompilerEngine::GetControlTypeName(uint8 ControlID)
{
	qstring retName;
	switch (ControlID)
	{
	case 0:
		retName = "PictureBox";
		break;
	case 1:
		retName = "Label";
		break;
	case 2:
		retName = "TextBox";
		break;
	case 3:
		retName = "Frame";
		break;
	case 4:
		retName = "CommandButton";
		break;
	case 5:
		retName = "CheckBox";
		break;
	case 6:
		retName = "OptionButton";
		break;
	case 7:
		retName = "ComboBox";
		break;
	case 8:
		retName = "ListBox";
		break;
	case 9:
		retName = "HScrollBar";
		break;
	case 0xA:
		retName = "VScrollBar";
		break;
	case 0xB:
		retName = "Timer";
		break;
	case 0xC:
		retName = "Print";
		break;
	case 0xD:
		retName = "Form";
		break;
	case 0xE:
		retName = "Screen";
		break;
	case 0xF:
		retName = "Clipboard";
		break;
	case 0x10:
		retName = "DriveListBox";
		break;
	case 0x11:
		retName = "DirListBox";
		break;
	case 0x12:
		retName = "FileListBox";
		break;
	case 0x13:
		retName = "Menu";
		break;
	case 0x14:
		retName = "MDIForm";
		break;
	case 0x16:
		retName = "Shape";
		break;
	case 0x17:
		retName = "Line";
		break;
	case 0x18:
		retName = "Image";
		break;
	case 0x25:
		retName = "Data";
		break;
	case 0x26:
		retName = "OLE";
		break;
	case 0x28:
		retName = "UserControl";
		break;
	case 0x29:
		retName = "PropertyPage";
		break;
	case 0x2A:
		retName = "UserDocument";
		break;
	case 0xFF:
		retName = "OLE";
		break;
	default:
		break;
	}


	return retName;
}

bool VBDecompilerEngine::hasOptionalObjectInfo(uint32 ObjType)
{
	switch (ObjType)
	{
	case 0x18001:		//Module
		return false;
	case 0x18021:
		return false;
	case 0x18041:
		return false;
	case 0x18061:
		return false;
	default:
		break;
	}

	return true;
}

bool VBDecompilerEngine::hasControl(uint32 ObjectType)
{
	switch (ObjectType)
	{
	case 0x18083:
		return true;
	case 0x180A3:
		return true;
	case 0x180C3:
		return true;
	case 0x1DA003:
		return true;
	case 0x1DA023:
		return true;
	case 0x1DA803:
		return true;
	case 0x1DE803:
		return true;
	case 0x3118843:
		return true;
	default:
		break;
	}

	return false;
}

void VBDecompilerEngine::GetSControl(uint32 sControlAddr, SControl& result)
{
	result.szNone1 = get_byte(sControlAddr++);
	result.szNone2 = get_byte(sControlAddr++);

	uint16 cLen = get_word(sControlAddr);
	sControlAddr = sControlAddr + 2;
	if (cLen != 0xFFFF)
	{
		result.ControlName = get_shortstring(sControlAddr);
		sControlAddr += cLen;
	}

	result.szNone3 = get_byte(sControlAddr++);
	result.ControlID = get_byte(sControlAddr++);

	return;
}


VBDecompilerEngine::VBDecompilerEngine()
{
	import_type(idati, -1, "IDispatchVtbl");
}

VBDecompilerEngine::~VBDecompilerEngine()
{

}

ea_t VBDecompilerEngine::GetUserCodeEndAddr()
{
	return VBHEAD.m_ProjectInfo.lpCodeEnd;
}

ea_t VBDecompilerEngine::GetUserCodeStartAddr()
{
	return VBHEAD.m_ProjectInfo.lpCodeStart;
}

void VBDecompilerEngine::SetSubMain()
{
	if (VBHEAD.m_lpSubMain)
	{
		set_name(VBHEAD.m_lpSubMain, "SubMain");
	}
}

void VBDecompilerEngine::SetEventFuncName()
{
	for (unsigned int nObjectIndex = 0; nObjectIndex < VBHEAD.mVec_ObjectTable.size(); ++nObjectIndex)
	{
		mid_ObjectMess& eObjectMess = VBHEAD.mVec_ObjectTable[nObjectIndex];

		if (!eObjectMess.bHasOptionalObj)
		{
			continue;
		}

		if (!eObjectMess.m_OptionObjInfo.bHasControl)
		{
			continue;
		}

		for (unsigned int eControlIndex = 0; eControlIndex < eObjectMess.m_OptionObjInfo.mVec_ControlInfo.size(); ++eControlIndex)
		{
			mid_ControlInfo& eControlInfo = eObjectMess.m_OptionObjInfo.mVec_ControlInfo[eControlIndex];

			if (!m_GuidMap[eControlInfo.m_ControlEventGuid].size())
			{
				continue;
			}

			qstring EventFuncName = eObjectMess.m_ObjectName + "::";
			EventFuncName += eControlInfo.m_ControlName + "::";

			qstring EventTypeName = m_GuidMap[eControlInfo.m_ControlEventGuid][0].StructName;
			EventFuncName += EventTypeName + "_";

			EventTypeName += "_vtbl";
			import_type(idati, -1, EventTypeName.c_str());
			tid_t idStruct = get_struc_id(EventTypeName.c_str());
			if (idStruct == BADADDR)
			{
				continue;
			}
			struc_t* pStruct = get_struc(idStruct);
			if (!pStruct)
			{
				continue;
			}

			for (unsigned int nEventIndex = 0; nEventIndex < eControlInfo.mVec_UseEvent.size(); ++nEventIndex)
			{
				if (eControlInfo.mVec_UseEvent[nEventIndex] == 0)
				{
					continue;
				}

				//第一个成员是继承的类,因此下标需要加一
				if (nEventIndex + 1 >= pStruct->memqty)
				{
					break;
				}

				insn_t JmpIns;
				decode_insn(&JmpIns, eControlInfo.mVec_UseEvent[nEventIndex] + 8);

				if (JmpIns.itype == NN_jmp && JmpIns.ops[0].type == o_near)
				{
					uint32 FuncAddr = JmpIns.ops[0].addr;
					qstring	EventName = get_member_name(pStruct->members[nEventIndex + 1].id);
					qstring SetName = EventFuncName + EventName;
					设置函数名称(FuncAddr, SetName.c_str());
				}
				else
				{
					msg("[VBDecompilerEngine]:SetEventFuncName,UnHandled EventType\n");
				}
			}
		}
	}
}

bool VBDecompilerEngine::Load_VBHpp()
{
	qstring hppPath = idadir(PLG_SUBDIR);
	hppPath += "\\COM\\";
	hppPath += VBHPPFILE;

	if (parse_decls(idati, hppPath.c_str(), NULL, HTI_LOWER | HTI_FIL))
	{
		return false;
	}

	FILE* fFile = qfopen(hppPath.c_str(), "r");
	if (!fFile)
	{
		return false;
	}

	//State为0是忽略数据
	//State为1是正在读CoClass
	//State为2是正在读Guid
	//State为3是正在读Struct
	unsigned int LastState = 0;
	qstring Line;
	int bStruct = false;

	coClassInfo coInfo;
	qstring InterfaceGuid;
	while (qgetline(&Line, fFile) != -1)
	{
		if (Line.find("//") != qstring::npos && Line.length() == 38)
		{
			if (LastState == 0)
			{
				coInfo.coClassGuid = Line.substr(2);
				LastState = 1;
				continue;
			}
			if (LastState == 1)
			{
				InterfaceGuid = Line.substr(2);
				LastState = 2;
				continue;
			}
			return false;
		}

		//Struct
		if (Line.find("struct ") == 0)
		{
			//VTable
			if (!bStruct)
			{
				if (LastState == 2)
				{
					LastState = 3;
					bStruct = true;
					continue;
				}
			}
			else
			{
				coInfo.StructName = Line.substr(7);
				coInfo.StructName.trim2();
				m_GuidMap[InterfaceGuid].push_back(coInfo);
				LastState = 3;
				bStruct = false;
				continue;
			}
			return false;
		}

		LastState = 0;
	}


	qfclose(fFile);
	return true;
}

void VBDecompilerEngine::AddClassGuid()
{
	for (unsigned int n = 0; n < VBHEAD.mVec_ObjectTable.size(); ++n)
	{
		mid_ObjectMess& eObjectMess = VBHEAD.mVec_ObjectTable.at(n);

		if (eObjectMess.bHasOptionalObj)
		{
			coClassInfo Info;

			qstring ObjectGuid = eObjectMess.m_ObjectIID;
			Info.coClassGuid = "00000000-0000-0000-0000-000000000000";
			Info.StructName = eObjectMess.m_ObjectName;
			m_GuidMap[ObjectGuid].push_back(Info);
		}
	}
}


bool VBDecompilerEngine::FlashComInterface()
{
	ea_t HresultCheckObj = GetHresultCheckObjAddr();

	//key值为函数地址,value值为当前函数的所有Obj数据
	std::map<ea_t, qvector<ObjData>> map_Obj;
	GetAllGuidXref(map_Obj, HresultCheckObj);

	for (std::map<ea_t, qvector<ObjData>>::iterator it = map_Obj.begin(); it != map_Obj.end(); it++)
	{
		if (!it->first)
		{
			continue;
		}
		func_t* pfunc = get_func(it->first);
		if (!pfunc)
		{
			continue;
		}
		HandleCheckObj(pfunc, it->second);
	}
	return true;
}

void VBDecompilerEngine::MakeFunction()
{
	for (unsigned int n = 0; n < VBHEAD.m_ProjectInfo.mVec_FuncTable.size(); ++n)
	{
		add_func(VBHEAD.m_ProjectInfo.mVec_FuncTable[n]);
	}

	//for (unsigned int nObjectIndex = 0; nObjectIndex < VBHEAD.mVec_ObjectTable.size(); ++nObjectIndex)
	//{
	//	mid_ObjectMess& ObjMess = VBHEAD.mVec_ObjectTable.at(nObjectIndex);

	//	qlist<qstring>::iterator it = ObjMess.mVec_MethodName.begin();
	//	for (it = ObjMess.mVec_MethodName.begin(); it != ObjMess.mVec_MethodName.end(); it++)
	//	{
	//		qstring test = *it;
	//		int ta = 0;
	//	}
	//	msg("%d\n", ObjMess.mVec_MethodName.size());
	//}
}

qstring VBDecompilerEngine::GetObjectTypeName(uint32 ObjType)
{
	//Module --- .bas
	//Form --- .frm
	//Class --- .cls
	//PropertyPage --- .pag
	//UserDocument --- .dob
	//UserControl --- .ctl

	qstring typeName;
	switch (ObjType)
	{
	case 0x18001:
		typeName = "Module";
		break;
	case 0x18083:
		typeName = "Form";
		break;
	case 0x118003:
		typeName = "Class";
		break;
	case 0x138003:
		typeName = "Class";
		break;
	case 0x158003:
		typeName = "PropertyPage";
		break;
	case 0x158803:
		typeName = "UserDocument";
		break;
	case 0x1DA003:
		typeName = "UserControl";
		break;
	default:
		break;
	}

	return typeName;
}


bool VBDecompilerEngine::LoadMethodLink(OptionalObjectInfo& opInfo, qvector<mid_VTable>& oVec_MethodLink)
{
	if (opInfo.iMethodLinkCount <= 0)
	{
		return true;
	}

	static std::map<qstring, midEnum_EventType> map_EventType;
	if (!map_EventType.size())
	{
		map_EventType["__imp_GetMemStr"] = e_GetMemStr;
		map_EventType["__imp_PutMemStr"] = e_PutMemStr;
		map_EventType["__imp_GetMem2"] = e_GetMem2;
		map_EventType["__imp_PutMem2"] = e_PutMem2;
		map_EventType["__imp_GetMem4"] = e_GetMem4;
		map_EventType["__imp_PutMem4"] = e_PutMem4;
		map_EventType["__imp_GetMemObj"] = e_GetMemObj;
		map_EventType["__imp_PutMemObj"] = e_PutMemObj;
		map_EventType["__imp_SetMemObj"] = e_SetMemObj;

		map_EventType["__imp_GetMemEvent"] = e_GetMemEvent;
		map_EventType["__imp_PutMemEvent"] = e_PutMemEvent;
		map_EventType["__imp_SetMemEvent"] = e_SetMemEvent;

		map_EventType["__imp_GetMemNewObj"] = e_GetMemNewObj;
		map_EventType["__imp_PutMemNewObj"] = e_PutMemNewObj;
		map_EventType["__imp_SetMemNewObj"] = e_SetMemNewObj;
	}

	uint32* pMethodLinkAddr = (uint32*)qalloc(opInfo.iMethodLinkCount * sizeof(uint32));
	get_bytes(pMethodLinkAddr, opInfo.iMethodLinkCount * sizeof(uint32), opInfo.MethodLinkTable);
	for (int16 nVtableIndex = 0; nVtableIndex < opInfo.iMethodLinkCount; ++nVtableIndex)
	{
		mid_VTable eVTable;
		insn_t tmpIns;
		uint32 decodeAddr = pMethodLinkAddr[nVtableIndex];
		decode_insn(&tmpIns, decodeAddr);
		if (tmpIns.itype == NN_jmp && tmpIns.ops[0].type == o_near)
		{
			eVTable.m_EventType = e_UserEvent;
			eVTable.m_MethodAddr = tmpIns.ops[0].addr;
			oVec_MethodLink.push_back(eVTable);
			continue;
		}

		decode_insn(&tmpIns, decodeAddr + 0x8);
		// mov ecx,offset xxxMemStr
		if (tmpIns.itype == NN_mov && tmpIns.ops[0].reg == 1 && tmpIns.ops[1].type == o_imm)
		{
			insn_t insAddOffset;
			decode_insn(&insAddOffset, decodeAddr);
			if (insAddOffset.itype == NN_add && insAddOffset.ops[1].type == o_imm)
			{
				eVTable.m_Offset = insAddOffset.ops[1].value;
			}

			insn_t insMemEvent;
			decode_insn(&insMemEvent, tmpIns.ops[1].value);
			if (insMemEvent.itype == NN_jmpni && insMemEvent.ops[0].type == o_mem)
			{
				qstring StrFuncName;
				get_ea_name(&StrFuncName, insMemEvent.ops[0].addr, GN_SHORT);

				eVTable.m_EventType = map_EventType[StrFuncName];
				if (eVTable.m_EventType == e_NoneEvent)
				{
					msg("[VBDecompilerEngine]:Unknown MemMethod\n");
					qfree(pMethodLinkAddr);
					return false;
				}
				eVTable.m_MethodAddr = pMethodLinkAddr[nVtableIndex];
				oVec_MethodLink.push_back(eVTable);
				continue;
			}
			msg("[VBDecompilerEngine]:Unknown MemStrIns\n");
			qfree(pMethodLinkAddr);
			return false;
		}

		decode_insn(&tmpIns, decodeAddr + 0x13);
		//mov ecx,offset xxxMemEvent
		if (tmpIns.itype == NN_mov && tmpIns.ops[0].reg == 1 && tmpIns.ops[1].type == o_imm)
		{
			insn_t insAddOffset;
			decode_insn(&insAddOffset, decodeAddr + 1);
			if (insAddOffset.itype == NN_add && insAddOffset.ops[1].type == o_imm)
			{
				eVTable.m_Offset = insAddOffset.ops[1].value;
			}

			insn_t insMemEvent;
			decode_insn(&insMemEvent, tmpIns.ops[1].value);
			if (insMemEvent.itype == NN_jmpni && insMemEvent.ops[0].type == o_mem)
			{
				qstring EventFuncName;
				get_ea_name(&EventFuncName, insMemEvent.ops[0].addr, GN_DEMANGLED);

				eVTable.m_EventType = map_EventType[EventFuncName];
				if (eVTable.m_EventType == e_NoneEvent)
				{
					msg("[VBDecompilerEngine]:Unknown MemEventMethod\n");
					qfree(pMethodLinkAddr);
					return false;
				}
				eVTable.m_MethodAddr = pMethodLinkAddr[nVtableIndex];
				oVec_MethodLink.push_back(eVTable);
				continue;
			}
			msg("[VBDecompilerEngine]:Unknown MemEventIns\n");
			qfree(pMethodLinkAddr);
			return false;
		}

		msg("[VBDecompilerEngine]:Unknown MethodLinkIns\n");
		qfree(pMethodLinkAddr);
		return false;
	}

	qfree(pMethodLinkAddr);
	return true;
}

bool VBDecompilerEngine::VBDE_ExternalControl(uint32 lpExControlAddr, uint32 cCount)
{
	ea_t ExternalControlStartAddr = lpExControlAddr;
	for (unsigned int n = 0; n < cCount; ++n)
	{
		ExternalControlInfo exControlInfo;
		get_bytes(&exControlInfo, sizeof(exControlInfo), ExternalControlStartAddr);

		mid_ExternalControl control;
		control.LibName = get_shortstring(ExternalControlStartAddr + exControlInfo.LibNameOffset);
		control.IDEName = get_shortstring(ExternalControlStartAddr + exControlInfo.ControlOffset);
		control.ControlName = get_shortstring(ExternalControlStartAddr + exControlInfo.ControlNameOffset);

		VBHEAD.mVec_ExternalControl.push_back(control);
		ExternalControlStartAddr += exControlInfo.NextControlOffset;
	}

	return true;
}

bool VBDecompilerEngine::VBDE_ComRegData(uint32 lpComRegDataAddr)
{
	COMRegData COMRegData;
	get_bytes(&COMRegData, sizeof(COMRegData), lpComRegDataAddr);
	if (COMRegData.bRegInfo)
	{
		RegistrationInfo regInfo;
		regInfo.bNextObject = COMRegData.bRegInfo;
		do
		{
			get_bytes(&regInfo, sizeof(RegistrationInfo), regInfo.bNextObject + lpComRegDataAddr);
			mid_COMRegData iReg;
			iReg.RegComName = get_shortstring(regInfo.bObjectName + lpComRegDataAddr);
			iReg.RegComUUID = charToUUID(regInfo.uuidObject);
			VBHEAD.mVec_ComRegData.push_back(iReg);
		} while (regInfo.bNextObject);
	}

	return true;
}

bool VBDecompilerEngine::TranslateNVBInfo(uint32 vbHeadAddr)
{
	VBHeader m_vbHeader;
	ProjectInfo m_projectInfo;
	ObjectTable m_objectTable;

	get_bytes(&m_vbHeader, sizeof(VBHeader), vbHeadAddr);
	get_bytes(&m_projectInfo, sizeof(ProjectInfo), m_vbHeader.lpProjectInfo);
	get_bytes(&m_objectTable, sizeof(ObjectTable), m_projectInfo.lpObjectTable);

	VBHEAD.m_lpSubMain = m_vbHeader.lpSubMain;
	VBHEAD.m_ProjectInfo.ProjectName = get_shortstring(vbHeadAddr + m_vbHeader.bSZProjectName);
	VBHEAD.m_ProjectInfo.ProjectDescription = get_shortstring(vbHeadAddr + m_vbHeader.bSZProjectDescription);

	VBHEAD.m_ProjectInfo.lpCodeStart = m_projectInfo.lpCodeStart;
	VBHEAD.m_ProjectInfo.lpCodeEnd = m_projectInfo.lpCodeEnd;


	//反编译注册的COM组件信息
	VBDE_ComRegData(m_vbHeader.lpComRegisterData);

	//额外的窗体控件
	VBDE_ExternalControl(m_vbHeader.lpExternalTable, m_vbHeader.wExternalCount);

	compiled_binpat_vec_t FuncHeadBin;
	parse_binpat_str(&FuncHeadBin, 0, "55 8B EC 83 EC", 16);
	uint32 SearchStartAddr = m_projectInfo.lpCodeStart;
	while (true)
	{
		SearchStartAddr = bin_search2(SearchStartAddr, m_projectInfo.lpCodeEnd, FuncHeadBin, 0x0);
		if (SearchStartAddr == BADADDR)
		{
			break;
		}
		VBHEAD.m_ProjectInfo.mVec_FuncTable.push_back(SearchStartAddr);
		SearchStartAddr = SearchStartAddr + 5;
	}

	//Decompiling API calls...
	for (unsigned int n = 0; n < m_projectInfo.dwExternalCount; ++n)
	{
		uint32 ExternalType = get_dword(m_projectInfo.lpExternalTable + 8 * n);
		uint32 lpExternalData = get_dword(m_projectInfo.lpExternalTable + 8 * n + 4);
		if (ExternalType == 6)
		{
			//COM组件的引用
			mid_ComApi comApi;
			comApi.uuid_coclass = get_uuid(get_dword(lpExternalData));
			comApi.GlobalVarAddr = get_dword(lpExternalData + 4);
			//msg("[COM]:%s--%a\n", comApi.uuid_coclass.c_str(), comApi.GlobalVarAddr);
			VBHEAD.m_ProjectInfo.m_ApiInfo.mVec_ComApi.push_back(comApi);
		}
		else if (ExternalType == 7)
		{
			//Api的引用
			mid_WindowsApi winApi;
			winApi.LibraryName = get_shortstring(get_dword(lpExternalData));
			winApi.FunctionName = get_shortstring(get_dword(lpExternalData + 4));
			VBHEAD.m_ProjectInfo.m_ApiInfo.mVec_WinApi.push_back(winApi);
		}
		else
		{
			msg("[VBDecompilerEngine]:Unknown ExternalApiType\n");
		}
	}

	if (m_objectTable.dwTotalObjects)
	{
		ObjectDescription* array_ObjectDescription = (ObjectDescription*)qalloc(m_objectTable.dwTotalObjects * sizeof(ObjectDescription));
		get_bytes(array_ObjectDescription, sizeof(ObjectDescription) * m_objectTable.dwTotalObjects, m_objectTable.lpObjectArray);

		for (unsigned int nObjectIndex = 0; nObjectIndex < m_objectTable.dwTotalObjects; ++nObjectIndex)
		{
			ObjectDescription eObjectDescription = array_ObjectDescription[nObjectIndex];
			mid_ObjectMess eObjMess;

			ObjectInfo tmpObjectInfo;
			get_bytes(&tmpObjectInfo, sizeof(tmpObjectInfo), eObjectDescription.ObjectInfo);

			PrivateObjectInfo tmpPrivateObjectInfo;
			get_bytes(&tmpPrivateObjectInfo, sizeof(tmpPrivateObjectInfo), tmpObjectInfo.lpPrivateObject);

			eObjMess.m_ObjectType = eObjectDescription.ObjectType;
			eObjMess.m_ObjectName = get_shortstring(eObjectDescription.NTSObjectName);
			eObjMess.m_ObjectTypeName = GetObjectTypeName(eObjectDescription.ObjectType);

			eObjMess.m_ClassSize = get_word(eObjectDescription.PublicBytes + 2);
			if (eObjectDescription.MethodCount > 0)
			{
				uint32* pMethodNameTable = (uint32*)qalloc(eObjectDescription.MethodCount * sizeof(uint32));
				get_bytes(pMethodNameTable, sizeof(uint32) * eObjectDescription.MethodCount, eObjectDescription.MethodNameTable);
				uint32* pParentObjTable = (uint32*)qalloc(eObjectDescription.MethodCount * sizeof(uint32));
				get_bytes(pParentObjTable, sizeof(uint32) * eObjectDescription.MethodCount, tmpPrivateObjectInfo.lpObjectList);

				//初始化偏移表
				qvector<uint16> vec_Offset;
				qvector<uint16> vec_UnfilterIndex;
				unsigned int SkipIndex = 0;
				for (int nMethodNameIndex = 0; nMethodNameIndex < eObjectDescription.MethodCount; ++nMethodNameIndex)
				{
					if (!pParentObjTable[nMethodNameIndex])
					{
						continue;
					}
					ParentObjInfo tmpParentObjInfo;
					get_bytes(&tmpParentObjInfo, sizeof(ParentObjInfo), pParentObjTable[nMethodNameIndex]);
					uint16 temp = tmpParentObjInfo.offset;
					vec_Offset.push_back(tmpParentObjInfo.offset);
					vec_UnfilterIndex.push_back(nMethodNameIndex);
				}

				if (vec_Offset.size())
				{
					uint16 uFirstOff = vec_Offset[0];
					for (int nMethodNameIndex = 0; nMethodNameIndex < eObjectDescription.MethodCount; ++nMethodNameIndex)
					{
						if (!pParentObjTable[nMethodNameIndex])
						{
							continue;
						}
						int FilterIndex = vec_Offset.index(uFirstOff);
						if (FilterIndex == -1)
						{
							break;
						}
						uFirstOff += 4;
						uint32 nameAddr = pMethodNameTable[vec_UnfilterIndex[FilterIndex]];
						qstring qMethodName = get_shortstring(nameAddr);
						eObjMess.mVec_MethodName.push_back(qMethodName);
					}
				}

				qfree(pMethodNameTable);
				qfree(pParentObjTable);
			}

			OptionalObjectInfo tmpOptionalObjectInfo{};
			if (hasOptionalObjectInfo(array_ObjectDescription[nObjectIndex].ObjectType))
			{
				eObjMess.bHasOptionalObj = true;
				get_bytes(&tmpOptionalObjectInfo, sizeof(OptionalObjectInfo), array_ObjectDescription[nObjectIndex].ObjectInfo + 0x38);
				eObjMess.m_ObjectIID = get_uuid(get_dword(tmpOptionalObjectInfo.aObjectDefaultIIDTable));
				eObjMess.m_iPCodeCount = tmpOptionalObjectInfo.iPCodeCount;
			}

			//msg("ObjName:%s,MethodNameCount:%d,MethodNameAddr:%a\n", eObjMess.m_ObjectName.c_str(), eObjectDescription.MethodCount, eObjectDescription.MethodNameTable);
			//msg("ObjName:%s,MethodLinkCount:%d,MethodLinkAddr:%a\n", eObjMess.m_ObjectName.c_str(), tmpOptionalObjectInfo.iMethodLinkCount, tmpOptionalObjectInfo.MethodLinkTable);
			//msg("ObjName:%s,PrivateObj:%a\n", eObjMess.m_ObjectName.c_str(), tmpPrivateObjectInfo.lpObjectList);


			//判断一个Object是否有控件
			if (hasControl(array_ObjectDescription[nObjectIndex].ObjectType))
			{
				eObjMess.m_OptionObjInfo.bHasControl = true;
			}

			if (tmpOptionalObjectInfo.lControlCount)
			{
				ControlInfo* array_ControlInfo = new ControlInfo[tmpOptionalObjectInfo.lControlCount];
				get_bytes(array_ControlInfo, sizeof(ControlInfo) * tmpOptionalObjectInfo.lControlCount, tmpOptionalObjectInfo.aControlArray);
				for (unsigned int n = 0; n < tmpOptionalObjectInfo.lControlCount; ++n)
				{
					mid_ControlInfo eControlInfo;

					eControlInfo.m_ControlName = get_shortstring(array_ControlInfo[n].lpszName);

					DefaultEvent defaultEvent;
					get_bytes(&defaultEvent, sizeof(DefaultEvent), array_ControlInfo[n].aEventTable);

					for (unsigned int nEventIndex = 0; nEventIndex < array_ControlInfo[n].UserEventCount; ++nEventIndex)
					{
						uint32 EventAddr = get_dword(array_ControlInfo[n].aEventTable + 0x18 + 0x4 * nEventIndex);
						eControlInfo.mVec_UseEvent.push_back(EventAddr);
					}

					//忘记这个字段是干什么的了...
					uint32 eCount = get_dword(defaultEvent.lpObjectInfo + 0x58);

					eControlInfo.m_ControlEventGuid = get_uuid(array_ControlInfo[n].lpGuid);
					eObjMess.m_OptionObjInfo.mVec_ControlInfo.push_back(eControlInfo);
				}
				delete[] array_ControlInfo;
			}

			if (!LoadMethodLink(tmpOptionalObjectInfo, eObjMess.m_OptionObjInfo.mVec_MethodLink))
			{
				qfree(array_ObjectDescription);
				return false;
			}

			VBHEAD.mVec_ObjectTable.push_back(eObjMess);
		}

		qfree(array_ObjectDescription);
	}

	return true;
}

qstring GetOtherEventName(mid_VTable vTable)
{
	qstring ret;
	switch (vTable.m_EventType)
	{
	case e_PutMemEvent:
		ret.sprnt("PutMemEvent_%a", vTable.m_Offset);
		break;
	case e_GetMemEvent:
		ret.sprnt("GetMemEvent_%a", vTable.m_Offset);
		break;
	case e_SetMemEvent:
		ret.sprnt("SetMemEvent_%a", vTable.m_Offset);
		break;
	case e_PutMemStr:
		ret.sprnt("PutMemStr_%a", vTable.m_Offset);
		break;
	case e_GetMemStr:
		ret.sprnt("GetMemStr_%a", vTable.m_Offset);
		break;
	case e_GetMem2:
		ret.sprnt("GetMem2_%a", vTable.m_Offset);
		break;
	case e_PutMem2:
		ret.sprnt("PutMem2_%a", vTable.m_Offset);
		break;
	case e_PutMem4:
		ret.sprnt("PutMem4_%a", vTable.m_Offset);
		break;
	case e_GetMem4:
		ret.sprnt("GetMem4_%a", vTable.m_Offset);
		break;
	case e_PutMemObj:
		ret.sprnt("PutMemObj_%a", vTable.m_Offset);
		break;
	case e_GetMemObj:
		ret.sprnt("GetMemObj_%a", vTable.m_Offset);
		break;
	case e_SetMemObj:
		ret.sprnt("SetMemObj_%a", vTable.m_Offset);
		break;
	case e_GetMemNewObj:
		ret.sprnt("GetMemNewObj_%a", vTable.m_Offset);
		break;
	case e_PutMemNewObj:
		ret.sprnt("PutMemNewObj_%a", vTable.m_Offset);
		break;
	case e_SetMemNewObj:
		ret.sprnt("SetMemNewObj_%a", vTable.m_Offset);
		break;
	default:
		break;
	}

	return ret;
}

void VBDecompilerEngine::CreateVTable()
{
	for (unsigned int nObjectIndex = 0; nObjectIndex < VBHEAD.mVec_ObjectTable.size(); ++nObjectIndex)
	{
		mid_ObjectMess eObjMess = VBHEAD.mVec_ObjectTable.at(nObjectIndex);
		qstring ObjName = eObjMess.m_ObjectName;

		if (!eObjMess.m_OptionObjInfo.mVec_MethodLink.size())
		{
			//这是不存在虚表的函数
			continue;
		}

		qvector<qstring> vec_MethodName;
		for (unsigned int nMethodIdx = 0; nMethodIdx < eObjMess.m_OptionObjInfo.mVec_MethodLink.size(); ++nMethodIdx)
		{
			qstring eName;
			mid_VTable& vTable = eObjMess.m_OptionObjInfo.mVec_MethodLink.at(nMethodIdx);

			if (vTable.m_EventType == e_UserEvent)
			{
				qstring qMethodName;
				if (eObjMess.mVec_MethodName.size())
				{
					qMethodName = eObjMess.mVec_MethodName.front();
					eObjMess.mVec_MethodName.pop_front();
					eName.sprnt("%s_%a", qMethodName.c_str(), vTable.m_MethodAddr);
				}
				else
				{
					eName.sprnt("sub_%a", vTable.m_MethodAddr);
				}
				qstring qSetName = ObjName + "::" + eName;
				set_name(vTable.m_MethodAddr, qSetName.c_str(), SN_NOWARN);
				vec_MethodName.push_back(eName);
			}
			else
			{
				eName = GetOtherEventName(vTable);
				vec_MethodName.push_back(eName);
			}
		}

		InitClassStructure(ObjName, vec_MethodName, eObjMess.m_iPCodeCount, eObjMess.m_ClassSize);
	}
}

bool VBDecompilerEngine::DoDecompile(ea_t PEEntry)
{
	unsigned char peEntryBytes[0xA];
	get_bytes(peEntryBytes, 0xA, PEEntry);

	if (peEntryBytes[0] != 0x68 && peEntryBytes[5] != 0xE8)
	{
		msg("[VBDecompilerEngine]:unHandled peEntry,To do...\n");
		return false;
	}

	uint32 aVBHeaderAddr = get_dword(PEEntry + 1);
	if (aVBHeaderAddr == BADADDR)
	{
		msg("[VBDecompilerEngine]:This is not a VB6 program\n");
		return false;
	}

	uint32 mask = get_dword(aVBHeaderAddr);
	if (mask == 0xB6543581)
	{
		msg("[VBDecompilerEngine]:P-Code,To do...\n");
		return false;
	}

	if (!TranslateNVBInfo(aVBHeaderAddr))
	{
		msg("[VBDecompilerEngine]:Init NativeVBInfo Failed...\n");
		return false;
	}


	return true;
}