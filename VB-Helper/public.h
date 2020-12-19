#pragma once
#include <typeinf.hpp>

extern til_t* idati;

struct coClassInfo
{
	qstring coClassGuid;
	qstring StructName;
};

qstring get_uuid(ea_t uuidAddr);

qstring get_shortstring(ea_t ea);

qstring charToUUID(uint8* pChar);