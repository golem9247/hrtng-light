/*
    Copyright © 2017-2024 AO Kaspersky Lab

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.

    Author: Sergey.Belov at kaspersky.com
*/

#include "warn_off.h"
#include <hexrays.hpp>
#include "warn_on.h"

#include "helpers.h"
#include "rename.h"

#define MAX_NAME_LEN 63 //(inf.max_autoname_len)

static bool isIdaInternalComment(const char* comment)
{
	if (!strncmp(comment, "jumptable", 9)) //jumptable 0040D4DD case 1
		return true;
	if (!strncmp(comment, "Exception filter", 16)) //Exception filter 0 for function 40AA7D
		return true;
	if (!strncmp(comment, "Exception handler", 17)) //Exception handler 0 for function 40AA7D
		return true;
	if (!strncmp(comment, "Finally handler", 15)) //Finally handler 0 for function 40B547
		return true;
	if (!strncmp(comment, "Microsoft VisualC", 17)) //Microsoft VisualC 2-14/net runtime
		return true;
	if (!strcmp(comment, "CRYPTO"))
		return true;
	return false;
}

static const char* badVarNames[] = {
  "this", "result", "Mem", "Memory", "Block", "String", "ProcName", "ProcAddress", "LibFileName", "ModuleName", "LibraryA", "LibraryW"
};

//for Vars and Args only (globals and struct members have own checks)
static bool isNameGood1(const char* name)
{
	if(*name == 0)
		return false;

	const char* vname = NULL;
	if ((name[0] == 'a' || name[0] == 'v')) {
		if((name[1] == 'r' && name[2] == 'g' && name[3] == '_') ||
			 (name[1] == 'a' && name[2] == 'r' && name[3] == '_'))
			vname = name + 4;
		else
			vname = name + 1;
	}
	if(vname &&
	  (vname[0] == 0 ||   (vname[0] >= '0' && vname[0] <= '9' &&
		(vname[1] == 0 || (((vname[1] >= '0' && vname[1] <= '9') || vname[1] == 'a') && //smth like 'a22' or 'a2a'
		(vname[2] == 0 ||   (vname[2] >= '0' && vname[2] <= '9' &&
	   vname[3] == 0)))))))
		return false;

#if IDA_SDK_VERSION == 760
	//annoing p_fld_xx renaming with ida 7.6
	if(name[0] == 'p' && name[1] == '_')
		name += 2;
	if(!strncmp(name, "fld_", 4))
		return false;
#endif

	//register name with optional suffix (ex: ecx0)
	size_t nlen = qstrlen(name);
	if(nlen > 1 && nlen <= 4) {
		const char* n = name;
		if(n[0] == 'e' || (is64bit() && n[0] == 'r' && !qisdigit(n[1]))) {
			n++; //ph.reg_names doesnt contain rax, eax form
			nlen--;
		}
		for(int32 i = 0; i < ph.regs_num; i++) {
			const char *r = ph.reg_names[i];
			size_t rlen = qstrlen(r);
			if(nlen >= rlen && strneq(n, r, rlen) &&
			   (n[rlen] == 0 ||
			    (qisdigit(n[rlen]) && n[rlen + 1] == 0) ||
			    (qisdigit(n[rlen]) && qisdigit(n[rlen + 1]) && n[rlen + 2] == 0)))
				return false;
		}
	}
	if((name[0] == 'F' || name[0] == 'B') && !qstrcmp(name + 1, "link"))
		return false;

	// names beginning from 'lp'
	if(name[0] == 'l' && name[1] == 'p' && name[2] != 0)
		name += 2;

	for(size_t i = 0; i < qnumber(badVarNames); i++)
		if(!qstrcmp(name, badVarNames[i]))
			return false;
	return true;
}

//there are additianal restrictions for names in call arguments
static const char* badArgNames[] = {
	"Str", "Src", "Dst", "dwBytes"
};
static bool isNameGood2(const char* name)
{
	if (!isNameGood1(name))
		return false;
	for(size_t i = 0; i < qnumber(badArgNames); i++)
		if(!qstrcmp(name, badArgNames[i]))
		return false;
	return true;
}

// consider "call(a, b)" is equal to assingment "a = b"
static bool isCallAssignName(const char* name, size_t* left, size_t* right)
{
	if (!namecmp(name, "strcpy") ||
			!namecmp(name, "wcscpy") ||
			!namecmp(name, "lstrcpy") ||
			!namecmp(name, "qmemcpy")
			) {
		*left = 0; *right = 1;
		return true;
	}
	return false;
}

static bool getCallName(cfunc_t *func, cexpr_t* call, qstring* name)
{
	if (call->op != cot_call)
		return false;

	qstring funcname;
	if (!getExpName(func, call->x, &funcname))
		return false;

	if (funcname.length() < 4) ///!!!!!! later check relay on this
		return false;

	if (!namecmp(funcname.c_str(), "GetLastError")) {
		*name = "err";
		return true;
	}

	size_t get = funcname.find("__get");
	if(get != qstring::npos && funcname.length() - get > 7 ) { //  strlen("::get") + 2
		*name = funcname.substr(get + 5);
		return true;
	}

	carglist_t &args = *call->a;
	if (!args.size())
		return false;

	if (!namecmp(funcname.c_str(), "LoadLibrary") ||
	    !namecmp(funcname.c_str(), "GetModuleHandle")) {
		qstring argName;
		if (args.size() == 1 && getExpName(func, &args[0], &argName)) {
			*name = "h";
			*name += argName;
			return true;
		}
	}
	else if (!namecmp(funcname.c_str(), "GetProcAddress")) {
		if (args.size() == 2 && getExpName(func, &args[1], name))
			return true;
	}
	else if (!namecmp(funcname.c_str(), "strdup") || !namecmp(funcname.c_str(), "wcsdup")) {
		if (args.size() == 1 && getExpName(func, &args[0], name))
			return true;
	}
	else if (!namecmp(funcname.c_str() + 4, "code_pointer") || !namecmp(funcname.c_str() + 2, "codePointer")) {
		if (args.size() == 1 && getExpName(func, &args[0], name))
			return true;
	}

	return false;
}

static bool getEaName(ea_t ea, qstring* name)
{
	flags64_t flg = get_flags(ea);
	if (has_user_name(flg)) {
		if (name) {
			get_ea_name(name, ea);
			stripName(name);
		}
		return true;
	}

	if(is_strlit(flg)) {
		if (name) {
			opinfo_t oi;
			if (!get_opinfo(&oi, ea, 0, flg))
				oi.strtype = STRTYPE_C;
			if(get_strlit_contents(name, ea, (size_t)(get_item_end(ea) - ea), oi.strtype, NULL, STRCONV_ESCAPE) > 0 ) {
				if (name->size() > MAX_NAME_LEN)
					name->resize(MAX_NAME_LEN);
				if(!validate_name(name, VNT_IDENT)) {
					msg("[hrt] FIXME: getEaName(%a, \"%s\")\n", ea, name->c_str());
					return false;
				}
			}
		}
		return true;
	}
		
	if (has_auto_name(flg)) {
		if (name) {
			get_ea_name(name, ea);
			stripName(name);
		}
		return true;
	}

	//get sub_xxx as well
	if(is_code(flg) && has_dummy_name(flg)) {
		qstring n = get_short_name(ea);
		if(!strncmp(n.c_str(), "sub_", 4)) {
			if(name) {
				//n.insert('p');
				*name = n;
			}
			return true;
		}
	}
	return false;
}

static bool renameEa(ea_t refea, const char* funcname, ea_t ea, const qstring* name)
{
  if (!is_mapped(ea))
		return false;
  qstring newName = *name;
	if (newName.size() > MAX_NAME_LEN)
		newName.resize(MAX_NAME_LEN);
	if(!validate_name(&newName, VNT_IDENT)) {
		msg("[hrt] FIXME: renameEa(%a, \"%s\")\n", refea, newName.c_str());
		return false;
	}

	if (set_name(ea, newName.c_str(), SN_NOCHECK | SN_AUTO | SN_NOWARN | SN_FORCE)) {
		make_name_auto(ea);
	}
  if (!has_cmt(get_flags(ea)) && newName != *name)
		set_cmt(ea, name->c_str(), true);
	msg("[hrt] %a %s: Global at %a was renamed to \"%s\"\n", refea, funcname, ea, newName.c_str());
	return true;
}

bool getVarName(lvar_t * var, qstring* name)
{
	if (!var->has_user_name() && !var->has_nice_name())
		return false;
	if(!isNameGood2(var->name.c_str()))
		return false;
	if (name) {
		*name = var->name;
		stripName(name);
	}
	return true;
}

bool renameVar(ea_t refea, const char* funcname, cfunc_t *func, ssize_t varIdx, const qstring* name, vdui_t *vdui)
{
	lvars_t *vars = func->get_lvars();
	lvar_t * var= &vars->at(varIdx);
	qstring newName = *name;
	if (newName.size() > MAX_NAME_LEN)
		newName.resize(MAX_NAME_LEN);
	if(!validate_name(&newName, VNT_IDENT)) {
		msg("[hrt] FIXME: renameVar(%a, \"%s\")\n", refea, newName.c_str());
		return false;
	}

	//check if proc doesnt already has such name
	bool acceptName = false;
	qstring basename = newName;
	for(int i = 0; i < 20; i++) {
		lvars_t::iterator it = vars->begin();
		for(; it != vars->end(); it++) {
			if(it->name == newName) {
				if(it == var) //why it tries rename again?
					return false;
				newName = basename;
				newName.cat_sprnt("_%d", i + 1);
				break;
			}
		}
		if(it == vars->end()) {
			acceptName = true;
			break;
		}
	}
	if(acceptName) {
		//if var is arg it will be useful to rename argument inside function prototype
		if(var->is_arg_var()) {
			int vIdx = (int)varIdx;
			ssize_t argIdx = func->argidx.index(vIdx);
			if(argIdx != -1) {
				tinfo_t funcType;
				if(func->get_func_type(&funcType)) {
					func_type_data_t fi;
					if(funcType.get_func_details(&fi) && fi.size() > (size_t)argIdx) {
						//msg("[hrt] %a %s: Rename arg%d \"%s\" to \"%s\"\n", refea, funcname, argIdx + 1, fi[argIdx].name.c_str(), newName.c_str());
						fi[argIdx].name = newName;
						tinfo_t newFType;
						newFType.create_func(fi);
						if(newFType.is_correct() && apply_tinfo(func->entry_ea, newFType, TINFO_DEFINITE)) {
							qstring typeStr;
							newFType.print(&typeStr);
							msg("[hrt] %a %s: Function prototype was recast for change arg-name into \"%s\"\n", refea, funcname, typeStr.c_str());
						}
					}
				}
			}
		}
		msg("[hrt] %a %s: Var \"%s\" was renamed to \"%s\"\n", refea, funcname, var->name.c_str(), newName.c_str());
		if (vdui) {
			vdui->rename_lvar(var, newName.c_str(), true); // vdui->rename_lvar can rebuild all internal structures/ call later!!!
		} else {
			//this way of renaming/retyping is not stored in databese, use:
			//restore_user_lvar_settings save_user_lvar_settings modify_user_lvars
			///< use mbl_array_t::set_nice_lvar_name() and
			///< mbl_array_t::set_user_lvar_name() to modify it
			//
			//CHECKME! mba->set_lvar_name(lvar_t &v, const char *name, int flagbits);  appeared in ida8.3
			var->name = newName;
			var->set_user_name();

			if(!var->has_user_type()) {
				tinfo_t t = getType4Name(newName.c_str());
				if(!t.empty() && var->accepts_type(t))
					if(var->set_lvar_type(t, true))
						msg("[hrt] %a %s: type of var '%s' refreshed\n", refea, funcname, newName.c_str());
			}

		}
		return true;
	}
	return false;
}

static bool getUdtMembName(tinfo_t type, uint32 offset, qstring* name)
{
//	qstring typeStr;
//	type.print(&typeStr);
//	msg("[hrt] getUdtMembName %x of \"%s\"\n", offset, typeStr.c_str());

	type.remove_ptr_or_array();
	if(!type.is_struct() && !type.is_union()) //t->is_decl_struct()
		return false;

	udm_t memb;
	memb.offset = offset;
	if(-1 == type.find_udm(&memb, STRMEM_AUTO))
		return false;

	if(memb.name[0] == 'f') {
		if(!strncmp(memb.name.c_str(), "field_",6))
			return false;
		if(!strncmp(memb.name.c_str(), "fld_",4))
			return false;
		if(!strncmp(memb.name.c_str(), "gap",3))
			return false;
	} else if(memb.name.length() == 2 && memb.name[0] == 'V' && memb.name[1] == 'T')
		return false;

	if (name) {
		*name = memb.name;
		stripName(name);
	}
	return true;
}

static bool renameUdtMemb(ea_t refea, const char* funcname, tinfo_t type, uint32 offset, qstring* name)
{
	type.remove_ptr_or_array();

	udm_t memb;
	memb.offset = offset;
	if(-1 == type.find_udm(&memb, STRMEM_AUTO)) {
		qstring typeStr;
		type.print(&typeStr);
		msg("[hrt] renameUdtMemb no %x offset inside \"%s\"\n", offset, typeStr.c_str());
		return false;
	}

	//"VT" handled in getUdtMembName as bad for name source, so disable "VT" autorenaming
	if(memb.name.length() == 2 && memb.name[0] == 'V' && memb.name[1] == 'T')
		return false;

#ifdef _DEBUG
	if (memb.name == *name) {
		msg("[hrt] %a: renameUdtMemb %s to self\n", refea, name->c_str());
		return false;
	}
#endif

	qstring newName = *name;
	if (newName.size() > MAX_NAME_LEN)
		newName.resize(MAX_NAME_LEN);
	if(!validate_name(&newName, VNT_UDTMEM)) {
		msg("[hrt] FIXME: renameUdtMemb(%a, ..., \"%s\")\n", refea, newName.c_str());
		return false;
	}

	//check if type already has such name
	qstring basename = newName;
	for(int i = 0; i < 20; i++) {
		udm_t m;
		m.name = newName;
		if(-1 == type.find_udm(&m, STRMEM_NAME)) {
			qstring typeStr;
			type.print(&typeStr);
			typeStr.append('.');
			typeStr.append(memb.name);
			struc_t* st = get_member_struc(typeStr.c_str());
			if(st && set_member_name(st, offset, newName.c_str())) {
				msg("[hrt] %a %s: struct \"%s\" member at 0x%x was renamed to %s\n", refea, funcname, typeStr.c_str(), offset, newName.c_str());
				return true;
			}
			//msg("[hrt] renameUdtMemb fail%d (struct \"%s\" member at 0x%x)\n", st ? 1 : 0, typeStr.c_str(), offset);
			return false;
		}
		newName = basename;
		newName.cat_sprnt("_%d", i + 1);
	}
	//msg("[hrt] renameUdtMemb fail2\n");
	return false;
}

//must return stripped name
bool getExpName(cfunc_t *func, cexpr_t* exp, qstring* name, bool derefPtr /* =false */)
{
	if(exp->op == cot_cast) //ignore typecast
		exp = exp->x;
	bool res = false;
	switch (exp->op)
	{
	case cot_helper:
		*name = exp->helper;
		res = true;
		break;
	case cot_str:
		*name = exp->string;
		res = true;
		break;
	case cot_var:
		return getVarName(&func->get_lvars()->at(exp->v.idx), name);
	case cot_obj:
		return getEaName(exp->obj_ea, name);
	case cot_memptr:
	case cot_memref:
		return getUdtMembName(exp->x->type, exp->m, name);
	case cot_call:
		return getCallName(func, exp, name);
	//case cot_fnum:
	case cot_num:
		{
			uint64 val = exp->numval();
			bool en = false;
			bool ok = false;
			if(exp->n->nf.is_enum()) {
				qstring &tn = exp->n->nf.type_name;
				if(tn != "MACRO_ERROR" && tn != "MACRO_STRSAFE") {
					en = true;
					//msg("[hrt] %a: enum name '%s'\n", exp->ea, tn.c_str());
				}
			} else if (/*val != 0*/ val > 1 && val < (uint64)0x80000000) {
				ok = true;
			}
			if (en || ok) {
				tinfo_t t = exp->type;
				exp->n->print(name, t);
				tag_remove(name);
			}
			if(ok) {
				stripNum(name); //strip "i64"/"ui64","LL"/"uLL" suffix
				name->insert('n');
				//name->append('_');
			}
			res = en || ok;
			break;
		}
	case cot_ref:
		if (derefPtr) {
			qstring ref;
			if(getExpName(func, exp->x, &ref)) {
				if(ref.length() > 1 && ref.last() == '_') {
					ref.remove_last();
					*name = ref;
				} else {
					qstring tname;
					if(!exp->x->type.get_type_name(&tname) || tname != ref) {
						*name = "p_";
						name->append(ref);
					}
				}
				res = true;
			}
		}
		break;
#if 0
	case cot_ptr:
		if (derefPtr) {
			if(exp->x->op == cot_cast){
				//here is not deref, check is it looks like type recast
				if(exp->x->type.is_ptr())
					return getExpName(func, exp->x->x, name);
			} else {
				qstring deref;
				if(getExpName(func, exp->x, &deref)) {
					if(deref.length() > 2 && deref[0] == 'p' && deref[1] == '_') {
						*name = deref.substr(2);
					} else {
						*name = "r_";
						name->append(deref);
					}
					res = true;
					msg("[hrt] %a: derefPtr ptr '%s'\n", exp->ea, name->c_str());
				}
			}
		}
		break;
#endif
	}
	if (res) {
		stripName(name);
		return name->length() != 0;
	}
	return res;
}


bool renameExp(ea_t refea, const char* funcname, cfunc_t *func, cexpr_t* exp, qstring* name, vdui_t *vdui, bool derefPtr /*= false*/)
{

	if(exp->op == cot_cast)
		exp = exp->x;

	if(derefPtr && exp->op == cot_ref &&
			(exp->x->op == cot_var || exp->x->op == cot_obj || exp->x->op == cot_memptr || exp->x->op == cot_memref)) {
		if (name->length() > 2 && name->at(0) == 'p' && name->at(1) == '_') {
			name->remove(0, 2);
		} else {
			name->append('_');
		}
		return renameExp(refea, funcname, func, exp->x, name);
	}

	//dirty hack for case when structure is placed on stack, and pointer to this struct is passed to func arg
	// ex:
	// strucA sA;
	// func(&sA);
	if(derefPtr && exp->op == cot_var) {
		lvars_t *vars = func->get_lvars();
		lvar_t * var = &vars->at(exp->v.idx);
		if(var->type().is_array()) {
			const type_t *namedType;
			if(get_named_type(NULL, name->c_str(), NTF_TYPE, &namedType) && is_type_struct(*namedType)) { // for structs NTF_TYPE flag req
				name->append('_');
				return renameVar(refea, funcname, func, exp->v.idx, name, vdui);
			}
		}
	}

	if(exp->op == cot_var)
		return renameVar(refea, funcname, func, exp->v.idx, name, vdui);
	if(exp->op == cot_obj)
		return renameEa(refea, funcname, exp->obj_ea, name);//, false);
	if(exp->op == cot_memptr || exp->op == cot_memref)
		return renameUdtMemb(refea, funcname, exp->x->type, exp->m, name);
	return false;
}

static tinfo_t getExpType(cfunc_t *func, cexpr_t* exp)
{
	if(!exp->type.empty())
		return exp->type;

	switch (exp->op)
	{
	case cot_var: 
		{
			lvars_t *vars = func->get_lvars();
			lvar_t * var = &vars->at(exp->v.idx);
			return var->type();
		}
	case cot_obj:
		{
			tinfo_t t;
			if(get_tinfo(&t, exp->obj_ea))
				return t;
			break;
		}
	case cot_memptr:
	case cot_memref:
		{
			tinfo_t struc = exp->x->type;
			struc.remove_ptr_or_array();
			if(struc.is_decl_struct()) {//is_struct())
				udm_t memb;
				memb.offset = exp->m;
				if(-1 != struc.find_udm(&memb, STRMEM_AUTO))
					return memb.type;
			}
			break;
		}
	case cot_ref:
		{
			tinfo_t t = getExpType(func, exp->x);
			if(!t.empty())
				return make_pointer(t);
			break;
		}
	case cot_ptr:
		{
			tinfo_t t = getExpType(func, exp->x);
			t = t.get_pointed_object();
			if(!t.empty())
				return t;
			break;
		}
	case cot_call:
		//TODO: not need now
		break;
	}

	exp->calc_type(false); //this doesn't work
	return exp->type;
}

#if 0
void dumpUserComments(ea_t entry_ea)
{
	user_cmts_t *cmts = restore_user_cmts(entry_ea);
	if ( cmts != NULL )
	{
		msg("[hrt] ------- %" FMT_Z " user defined comments\n", user_cmts_size(cmts));
		user_cmts_iterator_t p;
		for ( p=user_cmts_begin(cmts); p != user_cmts_end(cmts); p=user_cmts_next(p) )
		{
			const treeloc_t &tl = user_cmts_first(p);
			citem_cmt_t &cmt = user_cmts_second(p);
			msg("[hrt] Comment at %a, preciser %x:\n%s\n\n", tl.ea, tl.itp, cmt.c_str());
		}
		user_cmts_free(cmts);
	} else {
		msg("[hrt] ------- no user defined comments\n");
	}
}
#endif

#if 0 //def _DEBUG
	#define DEBUG_COMMENTS(args) {msg args;}
#else
	#define DEBUG_COMMENTS(args) {}
#endif

void autorename_n_pull_comments(cfunc_t *cfunc)
{
	struct ida_local cblock_visitor_t : public ctree_visitor_t
	{
		ea_t startea;
		qstring funcname;
		bool isProlog;
		cfunc_t *func;
		user_cmts_t *cmts;
		bool cmtModified;
		volatile bool varRenamed;
		bool scanCmts;
		uval_t stmtCnt;
		uval_t callCnt;
		qstring callProcName;

		cblock_visitor_t(cfunc_t *cfunc) : ctree_visitor_t(CV_PARENTS)
		{
			get_func_name(&funcname, cfunc->entry_ea);
			func = cfunc;
			cmts = restore_user_cmts(cfunc->entry_ea);
			if(cmts == NULL)
				cmts = user_cmts_new();
			cmtModified = false;
			startea = func->entry_ea; //BADADDR;
			isProlog = true;
			varRenamed = false;
			scanCmts = true;
			stmtCnt = 0;
			callCnt = 0;
		}

		~cblock_visitor_t()
		{
			//if(modified && user_cmts_size(cmts))
			//	save_user_cmts(func->entry_ea, cmts);
			if (cmtModified)
				func->save_user_cmts();
			user_cmts_free(cmts);
			//dumpUserComments(func->entry_ea);
		}

		//find comments in disasm and copy it 2 hexray
		int idaapi visit_insn(cinsn_t *ins)
		{
			if(!scanCmts) //only one pass
				return 0;

			if ( ins->op == cit_block && !isProlog) {//first block of proc
				DEBUG_COMMENTS(("block %a: %d\n", ins->ea, ins->op));
				startea = ins->ea;
				return 0;
			}
			//ida first block started from address of first operator, so we need skip first block startea override
			isProlog = false; 
			if(ins->op <= cit_block || //chks statements only
				ins->ea == BADADDR ) { // some statements have no address (ex: cit_break)
				DEBUG_COMMENTS(("skip %a: %d\n", ins->ea, ins->op));
				return 0;
			}
			stmtCnt++;

			if(startea > ins->ea)//ida doesn't make block statement after jump to return and startea is return
				startea = ins->ea;
			DEBUG_COMMENTS(("start %a ins %a: %d\n", startea, ins->ea, ins->op));
			ea_t ea = startea;
			treeloc_t loc;
			loc.ea = ins->ea;
			loc.itp = (ins->op == cit_expr)? ITP_SEMI : ITP_BLOCK1;
			DEBUG_COMMENTS(("%a: %d\n", ins->ea, ins->op));
			user_cmts_iterator_t it = user_cmts_find(cmts, loc);//get existing comments
			bool alredyCommented = it != user_cmts_end(cmts);

			//collect comments from disasm of statement
			//do not skip this loop if alredyCommented, because ea chain can be lost
			qstring comments;
			while (ea != BADADDR && ea <= ins->ea
#if IDA_SDK_VERSION >= 750
						 && func->mba->mbr.range_contains(ea)
#endif
						 ) {
				if(!alredyCommented && has_cmt(get_flags(ea))) {
					qstring cmt;
					if(get_cmt(&cmt, ea, true) != -1) {
						DEBUG_COMMENTS(("cmt at %a (ins %a) %s\n", ea, ins->ea, cmt.c_str()));
						if (!isIdaInternalComment(cmt.c_str()) && cmt[0] != ';') {
							if(comments.length())
								comments += "\n";
							comments += cmt;
						}
					}
				}
				ea = next_head(ea, BADADDR);
			}

			//set comments to hexrays
			if (!alredyCommented && comments.length()) {
				DEBUG_COMMENTS(("-- %a: %s\n", ins->ea, comments.c_str()));
				//citem_cmt_t c = comments.c_str();
				//user_cmts_insert(cmts, loc, c);
				func->set_user_cmt(loc, comments.c_str());
				cmtModified = true;
			}
			startea = ea;
			return 0;
		}

		//rename simple assignment parts
		int idaapi rename_asgn_sides(cexpr_t *asgn)
		{
			if(asgn->op != cot_asg && !is_relational(asgn->op))
				return 0;

			// find comments on this ea
			qstring comments;
			treeloc_t loc;
			loc.ea = asgn->ea;
			loc.itp = ITP_SEMI;
			user_cmts_iterator_t it = user_cmts_find(cmts, loc);//get existing comments
			if(it != user_cmts_end(cmts)) {
				comments = user_cmts_second(it);
			} else {
				loc.itp = ITP_BLOCK1;
				it = user_cmts_find(cmts, loc);
				if(it != user_cmts_end(cmts))
					comments = user_cmts_second(it);
			}
			//do not use comments been set by enum detector (see helpers.cpp appendComment)
			if (comments.length() && comments[0] == ';')
				comments.clear();

			//get name from right side of assignment
			qstring rname;
			cexpr_t* right = asgn->y;
			getExpName(func, right, &rname);

			//have some name on right side?
			bool renameLeft = false;
			if (comments.length() || rname.length()) {
				renameLeft = true;
				if(rname.length()) //assume rname more important then comments
					comments = rname;
			}

			//take name of left side assigned var
			//and rename it if possible
			qstring lname;
			cexpr_t* left = asgn->x;
			if(!getExpName(func, left, &lname) && renameLeft)
				varRenamed |= renameExp(asgn->ea, funcname.c_str(), func, left, &comments);

			//rename right if have good name on left side
			if(!rname.length() && (lname.length() || renameLeft)) {
				if(lname.length())//assume lname more important then comments
					comments = lname;
				varRenamed |= renameExp(asgn->ea, funcname.c_str(), func, right, &comments);
			}
			return 0;
		}

		int idaapi rename_call_params(cexpr_t *call)
		{
			if(call->op != cot_call)
				return 0;
			callCnt++;
			carglist_t &args = *call->a;
			if(!args.size())
				return 0;

			ea_t dstea;
			tinfo_t tif = getCallInfo(call, &dstea);

			bool bAllowTypeChange =  false;
			if(dstea != BADADDR && !tif.is_from_subtil()) {
				func_t *f = get_func(dstea);
				if(f && !(f->flags & FUNC_LIB)) {
#if 1
					//do check number of crefs for avoid renaming args in popular funcs like memcpy, alloc, etc
					uint32 nref = 0;
					for(ea_t xrefea = get_first_cref_to(dstea); xrefea != BADADDR && nref++ < 5; xrefea = get_next_cref_to(dstea, xrefea))
						;
					if(nref <= 4)
#endif
						bAllowTypeChange = true;
					//else msg("[hrt] %a %s: too many crefs to %a %s, do not change proto\n", call->ea, funcname.c_str(), dstea, get_short_name(dstea).c_str());
				}
			}

			if(!getExpName(func, call->x, &callProcName)) {
				//fix call to "off_xxx" (exports in debugger memory)
				cexpr_t *callDst = call->x;
				if(callDst->op == cot_cast)
					callDst = callDst->x;
				if(callDst->op == cot_obj) {
					flags64_t flg = get_flags(callDst->obj_ea);
					if(has_dummy_name(flg)) {
						qstring n = get_short_name(callDst->obj_ea);
						if(!strncmp(n.c_str(), "off_", 4)) {
							ea_t dest = get_ea(callDst->obj_ea);
							if(getEaName(dest, &callProcName)) {
								renameEa(call->ea, funcname.c_str(), callDst->obj_ea, &callProcName);
							}
						}
					}
				}
			}

			bool bCallAssign = false;
			qstring anL, anR;
			size_t  iL, iR;
			if(!callProcName.empty() && isCallAssignName(callProcName.c_str(), &iL, &iR))
				bCallAssign = true;

			tif.remove_ptr_or_array();
			if(tif.is_decl_func()) {
				func_type_data_t fi;
				//get_func_details(&fi, GTD_CALC_ARGLOCS) may cause INTERR 50689 on call cot_helper
				if(tif.get_func_details(&fi, bAllowTypeChange ? GTD_CALC_ARGLOCS : GTD_NO_ARGLOCS)) {
					size_t nArgs = fi.size();//??? check vararg(,...)
					if(nArgs > args.size()) 
						return 0;
					bool fiChanged = false;
					for(size_t i = 0; i < fi.size(); i++) {
						cexpr_t *arg = &args[i];
						qstring name = fi[i].name;
						stripName(&name);
						qstring argVarName;
						bool argNamed = getExpName(func, arg, &argVarName, true);

						if(argNamed && bCallAssign) {
							if(i == iL) anL = argVarName;
							if(i == iR) anR = argVarName;
						}

						if(!argNamed && isNameGood2(name.c_str())) {
								varRenamed |= renameExp(call->ea, funcname.c_str(), func, arg, &name, nullptr, true);
						} else if(argNamed && bAllowTypeChange && !isNameGood1(name.c_str())) {
								//msg("[hrt] %a %s: In function %a %s rename arg%d \"%s\" to \"%s\"\n", call->ea, funcname.c_str(), dstea, get_short_name(dstea).c_str(), i + 1, fi[i].name.c_str(), argVarName.c_str());
								fi[i].name = argVarName;
								fiChanged = true;
								if(!remove_pointer(fi[i].type).is_struct()) { //retrieve, compare and set type
									if(arg->op == cot_cast)
										arg = arg->x; //remove typecast
									tinfo_t argType = getExpType(func, arg);
									if(remove_pointer(argType).is_struct()) {
#if 0
										qstring oldType; fi[i].type.print(&oldType);
										qstring newType; argType.print(&newType);
										msg("[hrt] %a %s: In function %a %s recast arg%d %s from \"%s\" to \"%s\"\n", 
											call->ea, funcname.c_str(), dstea, get_short_name(dstea).c_str(), i + 1, fi[i].name.c_str(),
											oldType.c_str(), newType.c_str());
#endif
										fi[i].type = argType;
									}
								}
							
						}
					}
					if(bCallAssign) {
						if(!anL.length() && anR.length())
							varRenamed |= renameExp(call->ea, funcname.c_str(), func, &args[iL], &anR, nullptr, true);
						else if(anL.length() && !anR.length())
							varRenamed |= renameExp(call->ea, funcname.c_str(), func, &args[iR], &anL, nullptr, true);
					}
					if(fiChanged) {
						//TODO: some name cleanup, remove duplicates (?)
						tinfo_t newFType;
						newFType.create_func(fi);
						if(newFType.is_correct() && set_tinfo(dstea, &newFType)) //apply_tinfo(dstea, newFType, TINFO_DEFINITE)) or apply_callee_tinfo
						{
							qstring typeStr;
							newFType.print(&typeStr);
							msg("[hrt] %a %s: Function %a %s was recast to \"%s\"\n", call->ea, funcname.c_str(), dstea, get_short_name(dstea).c_str(), typeStr.c_str());
						}
					}
				}
			} else if(!tif.empty()){
				qstring typeStr;
				tif.print(&typeStr);
				msg("[hrt] %a: type \"%s\" is not function\n", call->ea, typeStr.c_str());
			}
			return 0;
		}

		//check for strings in data section, make comments
		int idaapi set_cmt_for_hidden_strings(cexpr_t *obj)
		{
			if (obj->op != cot_obj || obj->is_cstr()) //FIXME: is_cstr sometimes is true even if here is a "hidden string"
				return 0;

			ea_t ea = obj->obj_ea;
			flags64_t flg = get_flags(ea);
			if (!is_strlit(flg) && is_ea(flg)) {
				ea = get_ea(ea);
				flg = get_flags(ea);
			}
			if (!is_strlit(flg))
				return 0;

			treeloc_t loc;
			loc.ea = obj->ea;
			loc.itp = ITP_BLOCK1;
			citem_t *p = parent_expr();
			while (p && p->op <= cot_last && p->op != cot_call) {
				p = func->body.find_parent_of(p);
			}
			if (!p)
				p = parent_expr();
			if (p->op == cot_call) {
				carglist_t &args = *((cexpr_t*)p)->a;
				if (args.size() > 1) {
					for (size_t i = 0; i < args.size(); i++) {
						cexpr_t *arg = &args[i];
						if (arg == obj || 
							(arg->op == cot_cast && arg->x == obj)
							/*arg->contains_expr((cexpr_t const *)obj)*/) {
							if (args.size() != i + 1 && i < 63)
								loc.itp = item_preciser_t(ITP_ARG1 + i); //this doesnt works for last arg
							break;
						}
					}
				}
				if (loc.itp == ITP_BLOCK1) { //arg not found or last arg
					while (p && p->op <= cot_last)  p = func->body.find_parent_of(p);
					if (!p) p = parent_expr();
				}
			}
			if(p->op == cit_expr)
				loc.itp = ITP_SEMI;
			else if(p->op == cit_if)
				loc.itp = ITP_BRACE2;

			if (p->ea != BADADDR)
				loc.ea = p->ea;

			user_cmts_iterator_t it = user_cmts_find(cmts, loc);
			if (it != user_cmts_end(cmts)) 
				return 0;

			opinfo_t oi;
			qstring strcontent;
			if (!get_opinfo(&oi, ea, 0, flg))
				oi.strtype = STRTYPE_C;
			if (get_strlit_contents(&strcontent, ea, (size_t)(get_item_end(ea) - ea), oi.strtype, NULL, STRCONV_ESCAPE) > 0) {
				strcontent.insert('"');
				strcontent.append('"');
				func->set_user_cmt(loc, strcontent.c_str());
				cmtModified = true;
			}
			return 0;
		}
		
		int idaapi visit_expr(cexpr_t *exp)
		{
			if(exp->op == cot_call)
				return rename_call_params(exp);
			if(exp->op == cot_asg || is_relational(exp->op))
				return rename_asgn_sides(exp);
			if (exp->op == cot_obj)
				return set_cmt_for_hidden_strings(exp);
			return 0;
		}

		void apply_loop()
		{
			uint32 i = 0;
			for(; i < 10; ++i)
			{
				varRenamed = false;
				apply_to(&func->body, NULL);
				if(!varRenamed)
					break;
				scanCmts = false;
			}
			if(i >= 10) {
				msg("[hrt] %a %s WARNING: rename looping...\n", func->entry_ea , funcname.c_str());
			}
			
			//rename func itself if it has dummy name and only one statement inside
			if (stmtCnt == 1 && callCnt == 1 && has_dummy_name(get_flags(func->entry_ea))) {
				if (!callProcName.empty() && strncmp(callProcName.c_str(), "psub_", 5)) {
					qstring newName = callProcName;
					if (newName.size() > MAX_NAME_LEN - 1)
						newName.resize(MAX_NAME_LEN - 1);
					newName.append('_');
					if (validate_name(&newName, VNT_IDENT) && set_name(func->entry_ea, newName.c_str(), SN_AUTO | SN_NOWARN | SN_FORCE)) {
						make_name_auto(func->entry_ea);
						msg("[hrt] %s was renamed to \"%s\"\n", funcname.c_str(), newName.c_str());
					}
				}
			}
		}

	};
	cblock_visitor_t cbv(cfunc);
	cbv.apply_loop();
	//cfunc->verify(ALLOW_UNUSED_LABELS, false);
}