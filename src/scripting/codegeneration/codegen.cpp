/*
** thingdef_expression.cpp
**
** Expression evaluation
**
**---------------------------------------------------------------------------
** Copyright 2008 Christoph Oelckers
** All rights reserved.
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions
** are met:
**
** 1. Redistributions of source code must retain the above copyright
**    notice, this list of conditions and the following disclaimer.
** 2. Redistributions in binary form must reproduce the above copyright
**    notice, this list of conditions and the following disclaimer in the
**    documentation and/or other materials provided with the distribution.
** 3. The name of the author may not be used to endorse or promote products
**    derived from this software without specific prior written permission.
** 4. When not used as part of ZDoom or a ZDoom derivative, this code will be
**    covered by the terms of the GNU General Public License as published by
**    the Free Software Foundation; either version 2 of the License, or (at
**    your option) any later version.
**
** THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
** IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
** OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
** IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
** INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
** NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
** DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
** THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
** THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**---------------------------------------------------------------------------
**
*/

#include <stdlib.h>
#include "actor.h"
#include "sc_man.h"
#include "tarray.h"
#include "templates.h"
#include "cmdlib.h"
#include "i_system.h"
#include "m_random.h"
#include "a_pickups.h"
#include "thingdef.h"
#include "p_lnspec.h"
#include "doomstat.h"
#include "codegen.h"
#include "m_fixed.h"
#include "vmbuilder.h"
#include "v_text.h"
#include "w_wad.h"
#include "math/cmath.h"

extern FRandom pr_exrandom;
FMemArena FxAlloc(65536);

struct FLOP
{
	ENamedName Name;
	int Flop;
	double (*Evaluate)(double);
};

// Decorate operates on degrees, so the evaluate functions need to convert
// degrees to radians for those that work with angles.
static const FLOP FxFlops[] =
{
	{ NAME_Exp,		FLOP_EXP,		[](double v) { return g_exp(v); } },
	{ NAME_Log,		FLOP_LOG,		[](double v) { return g_log(v); } },
	{ NAME_Log10,	FLOP_LOG10,		[](double v) { return g_log10(v); } },
	{ NAME_Sqrt,	FLOP_SQRT,		[](double v) { return g_sqrt(v); } },
	{ NAME_Ceil,	FLOP_CEIL,		[](double v) { return ceil(v); } },
	{ NAME_Floor,	FLOP_FLOOR,		[](double v) { return floor(v); } },

	{ NAME_ACos,	FLOP_ACOS_DEG,	[](double v) { return g_acos(v) * (180.0 / M_PI); } },
	{ NAME_ASin,	FLOP_ASIN_DEG,	[](double v) { return g_asin(v) * (180.0 / M_PI); } },
	{ NAME_ATan,	FLOP_ATAN_DEG,	[](double v) { return g_atan(v) * (180.0 / M_PI); } },
	{ NAME_Cos,		FLOP_COS_DEG,	[](double v) { return g_cosdeg(v); } },
	{ NAME_Sin,		FLOP_SIN_DEG,	[](double v) { return g_sindeg(v); } },
	{ NAME_Tan,		FLOP_TAN_DEG,	[](double v) { return g_tan(v * (M_PI / 180.0)); } },

	{ NAME_CosH,	FLOP_COSH,		[](double v) { return g_cosh(v); } },
	{ NAME_SinH,	FLOP_SINH,		[](double v) { return g_sinh(v); } },
	{ NAME_TanH,	FLOP_TANH,		[](double v) { return g_tanh(v); } },
};

//==========================================================================
//
// FCompileContext
//
//==========================================================================

FCompileContext::FCompileContext(PFunction *fnc, PPrototype *ret, bool fromdecorate, int stateindex, int statecount, int lump) 
	: ReturnProto(ret), Function(fnc), Class(nullptr), FromDecorate(fromdecorate), StateIndex(stateindex), StateCount(statecount), Lump(lump)
{
	if (fnc != nullptr) Class = fnc->OwningClass;
}

FCompileContext::FCompileContext(PStruct *cls, bool fromdecorate) 
	: ReturnProto(nullptr), Function(nullptr), Class(cls), FromDecorate(fromdecorate), StateIndex(-1), StateCount(0), Lump(-1)
{
}

PSymbol *FCompileContext::FindInClass(FName identifier, PSymbolTable *&symt)
{
	return Class != nullptr? Class->Symbols.FindSymbolInTable(identifier, symt) : nullptr;
}

PSymbol *FCompileContext::FindInSelfClass(FName identifier, PSymbolTable *&symt)
{
	// If we have no self we cannot retrieve any values from it.
	if (Function == nullptr || Function->Variants[0].SelfClass == nullptr) return nullptr;
	return Function->Variants[0].SelfClass->Symbols.FindSymbolInTable(identifier, symt);
}
PSymbol *FCompileContext::FindGlobal(FName identifier)
{
	return GlobalSymbols.FindSymbol(identifier, true);
}

void FCompileContext::CheckReturn(PPrototype *proto, FScriptPosition &pos)
{
	assert(proto != nullptr);
	bool fail = false;

	if (ReturnProto == nullptr)
	{
		ReturnProto = proto;
		return;
	}

	// A prototype that defines fewer return types can be compatible with
	// one that defines more if the shorter one matches the initial types
	// for the longer one.
	if (ReturnProto->ReturnTypes.Size() < proto->ReturnTypes.Size())
	{ // Make proto the shorter one to avoid code duplication below.
		swapvalues(proto, ReturnProto);
	}
	// If one prototype returns nothing, they both must.
	if (proto->ReturnTypes.Size() == 0)
	{
		if (ReturnProto->ReturnTypes.Size() != 0)
		{
			fail = true;
		}
	}
	else
	{
		for (unsigned i = 0; i < proto->ReturnTypes.Size(); i++)
		{
			if (ReturnProto->ReturnTypes[i] != proto->ReturnTypes[i])
			{ // Incompatible
				fail = true;
				break;
			}
		}
	}

	if (fail)
	{
		pos.Message(MSG_ERROR, "Return type mismatch");
	}
}

bool FCompileContext::CheckReadOnly(int flags)
{
	if (!(flags & VARF_ReadOnly)) return false;
	if (!(flags & VARF_InternalAccess)) return true;
	return Wads.GetLumpFile(Lump) != 0;
}

FxLocalVariableDeclaration *FCompileContext::FindLocalVariable(FName name)
{
	if (Block == nullptr)
	{
		return nullptr;
	}
	else
	{
		return Block->FindLocalVariable(name, *this);
	}
}

static PStruct *FindStructType(FName name)
{
	PStruct *ccls = PClass::FindClass(name);
	if (ccls == nullptr)
	{
		ccls = dyn_cast<PStruct>(TypeTable.FindType(RUNTIME_CLASS(PStruct), 0, (intptr_t)name, nullptr));
		if (ccls == nullptr) ccls = dyn_cast<PStruct>(TypeTable.FindType(RUNTIME_CLASS(PNativeStruct), 0, (intptr_t)name, nullptr));
	}
	return ccls;
}
//==========================================================================
//
// ExpEmit
//
//==========================================================================

ExpEmit::ExpEmit(VMFunctionBuilder *build, int type, int count)
: RegNum(build->Registers[type].Get(count)), RegType(type), RegCount(count), Konst(false), Fixed(false), Final(false), Target(false)
{
}

void ExpEmit::Free(VMFunctionBuilder *build)
{
	if (!Fixed && !Konst && RegType <= REGT_TYPE)
	{
		build->Registers[RegType].Return(RegNum, RegCount);
	}
}

void ExpEmit::Reuse(VMFunctionBuilder *build)
{
	if (!Fixed && !Konst)
	{
		assert(RegCount == 1);
		bool success = build->Registers[RegType].Reuse(RegNum);
		assert(success && "Attempt to reuse a register that is already in use");
	}
}

//==========================================================================
//
// FindBuiltinFunction
//
// Returns the symbol for a decorate utility function. If not found, create
// it and install it a local symbol table.
//
//==========================================================================

static PSymbol *FindBuiltinFunction(FName funcname, VMNativeFunction::NativeCallType func)
{
	PSymbol *sym = GlobalSymbols.FindSymbol(funcname, false);
	if (sym == nullptr)
	{
		PSymbolVMFunction *symfunc = new PSymbolVMFunction(funcname);
		VMNativeFunction *calldec = new VMNativeFunction(func, funcname);
		calldec->PrintableName = funcname.GetChars();
		symfunc->Function = calldec;
		sym = symfunc;
		GlobalSymbols.AddSymbol(sym);
	}
	return sym;
}

//==========================================================================
//
//
//
//==========================================================================

static bool AreCompatiblePointerTypes(PType *dest, PType *source, bool forcompare = false)
{
	if (dest->IsKindOf(RUNTIME_CLASS(PPointer)) && source->IsKindOf(RUNTIME_CLASS(PPointer)))
	{
		// Pointers to different types are only compatible if both point to an object and the source type is a child of the destination type.
		auto fromtype = static_cast<PPointer *>(source);
		auto totype = static_cast<PPointer *>(dest);
		if (fromtype == nullptr) return true;
		if (!forcompare && totype->IsConst != fromtype->IsConst) return false;
		if (fromtype == totype) return true;
		if (fromtype->PointedType->IsKindOf(RUNTIME_CLASS(PClass)) && totype->PointedType->IsKindOf(RUNTIME_CLASS(PClass)))
		{
			auto fromcls = static_cast<PClass *>(fromtype->PointedType);
			auto tocls = static_cast<PClass *>(totype->PointedType);
			if (forcompare && tocls->IsDescendantOf(fromcls)) return true;
			return (fromcls->IsDescendantOf(tocls));
		}
	}
	return false;
}

//==========================================================================
//
//
//
//==========================================================================

ExpEmit FxExpression::Emit (VMFunctionBuilder *build)
{
	ScriptPosition.Message(MSG_ERROR, "Unemitted expression found");
	return ExpEmit();
}


//==========================================================================
//
//
//
//==========================================================================

bool FxExpression::isConstant() const
{
	return false;
}

//==========================================================================
//
//
//
//==========================================================================

VMFunction *FxExpression::GetDirectFunction()
{
	return nullptr;
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxExpression::Resolve(FCompileContext &ctx)
{
	isresolved = true;
	return this;
}

//==========================================================================
//
// Returns true if we can write to the address.
//
//==========================================================================

bool FxExpression::RequestAddress(FCompileContext &ctx, bool *writable)
{
	if (writable != nullptr) *writable = false;
	return false;
}

//==========================================================================
//
// Called by return statements.
//
//==========================================================================

PPrototype *FxExpression::ReturnProto()
{
	assert(ValueType != nullptr);

	TArray<PType *> ret(0);
	TArray<PType *> none(0);
	if (ValueType != TypeVoid)
	{
		ret.Push(ValueType);
	}

	return NewPrototype(ret, none);
}

//==========================================================================
//
//
//
//==========================================================================

static int EncodeRegType(ExpEmit reg)
{
	int regtype = reg.RegType;
	if (reg.Konst)
	{
		regtype |= REGT_KONST;
	}
	else if (reg.RegCount == 2)
	{
		regtype |= REGT_MULTIREG2;

	}
	else if (reg.RegCount == 3)
	{
		regtype |= REGT_MULTIREG3;
	}
	return regtype;
}

//==========================================================================
//
//
//
//==========================================================================

static int EmitParameter(VMFunctionBuilder *build, FxExpression *operand, const FScriptPosition &pos)
{
	ExpEmit where = operand->Emit(build);

	if (where.RegType == REGT_NIL)
	{
		pos.Message(MSG_ERROR, "Attempted to pass a non-value");
		build->Emit(OP_PARAM, 0, where.RegType, where.RegNum);
		return 1;
	}
	else
	{
		build->Emit(OP_PARAM, 0, EncodeRegType(where), where.RegNum);
		where.Free(build);
		return where.RegCount;
	}
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxConstant::MakeConstant(PSymbol *sym, const FScriptPosition &pos)
{
	FxExpression *x;
	PSymbolConstNumeric *csym = dyn_cast<PSymbolConstNumeric>(sym);
	if (csym != nullptr)
	{
		if (csym->ValueType->IsA(RUNTIME_CLASS(PInt)))
		{
			x = new FxConstant(csym->Value, pos);
		}
		else if (csym->ValueType->IsA(RUNTIME_CLASS(PFloat)))
		{
			x = new FxConstant(csym->Float, pos);
		}
		else
		{
			pos.Message(MSG_ERROR, "Invalid constant '%s'\n", csym->SymbolName.GetChars());
			return nullptr;
		}
	}
	else
	{
		pos.Message(MSG_ERROR, "'%s' is not a constant\n", sym->SymbolName.GetChars());
		x = nullptr;
	}
	return x;
}

ExpEmit FxConstant::Emit(VMFunctionBuilder *build)
{
	ExpEmit out;

	out.Konst = true;
	int regtype = value.Type->GetRegType();
	out.RegType = regtype;
	if (regtype == REGT_INT)
	{
		out.RegNum = build->GetConstantInt(value.Int);
	}
	else if (regtype == REGT_FLOAT)
	{
		out.RegNum = build->GetConstantFloat(value.Float);
	}
	else if (regtype == REGT_POINTER)
	{
		VM_ATAG tag = ATAG_GENERIC;
		if (value.Type == TypeState)
		{
			tag = ATAG_STATE;
		}
		else if (value.Type->GetLoadOp() == OP_LO)
		{
			tag = ATAG_OBJECT;
		}
		out.RegNum = build->GetConstantAddress(value.pointer, tag);
	}
	else if (regtype == REGT_STRING)
	{
		out.RegNum = build->GetConstantString(value.GetString());
	}
	else
	{
		ScriptPosition.Message(MSG_ERROR, "Cannot emit needed constant");
		out.RegNum = 0;
	}
	return out;
}

//==========================================================================
//
//
//
//==========================================================================

FxVectorValue::FxVectorValue(FxExpression *x, FxExpression *y, FxExpression *z, const FScriptPosition &sc)
	:FxExpression(EFX_VectorValue, sc)
{
	xyz[0] = x;
	xyz[1] = y;
	xyz[2] = z;
	isConst = false;
	ValueType = TypeVoid;	// we do not know yet
}

FxVectorValue::~FxVectorValue()
{
	for (auto &a : xyz)
	{
		SAFE_DELETE(a);
	}
}

FxExpression *FxVectorValue::Resolve(FCompileContext&ctx)
{
	bool fails = false;

	for (auto &a : xyz)
	{
		if (a != nullptr)
		{
			a = a->Resolve(ctx);
			if (a == nullptr) fails = true;
			else
			{
				if (a->ValueType != TypeVector2)	// a vec3 may be initialized with (vec2, z)
				{
					a = new FxFloatCast(a);
					a = a->Resolve(ctx);
					fails |= (a == nullptr);
				}
			}
		}
	}
	if (fails)
	{
		delete this;
		return nullptr;
	}
	// at this point there are three legal cases:
	// * two floats = vector2
	// * three floats = vector3
	// * vector2 + float = vector3
	if (xyz[0]->ValueType == TypeVector2)
	{
		if (xyz[1]->ValueType != TypeFloat64 || xyz[2] != nullptr)
		{
			ScriptPosition.Message(MSG_ERROR, "Not a valid vector");
			delete this;
			return nullptr;
		}
		ValueType = TypeVector3;
		if (xyz[0]->ExprType == EFX_VectorValue)
		{
			// If two vector initializers are nested, unnest them now.
			auto vi = static_cast<FxVectorValue*>(xyz[0]);
			xyz[2] = xyz[1];
			xyz[1] = vi->xyz[1];
			xyz[0] = vi->xyz[0];
			vi->xyz[0] = vi->xyz[1] = nullptr; // Don't delete our own expressions.
			delete vi;
		}
	}
	else if (xyz[0]->ValueType == TypeFloat64 && xyz[1]->ValueType == TypeFloat64)
	{
		ValueType = xyz[2] == nullptr ? TypeVector2 : TypeVector3;
	}
	else
	{
		ScriptPosition.Message(MSG_ERROR, "Not a valid vector");
		delete this;
		return nullptr;
	}

	// check if all elements are constant. If so this can be emitted as a constant vector.
	isConst = true;
	for (auto &a : xyz)
	{
		if (a != nullptr && !a->isConstant()) isConst = false;
	}
	return this;
}

static ExpEmit EmitKonst(VMFunctionBuilder *build, ExpEmit &emit)
{
	if (emit.Konst)
	{
		ExpEmit out(build, REGT_FLOAT);
		build->Emit(OP_LKF, out.RegNum, emit.RegNum);
		return out;
	}
	return emit;
}

ExpEmit FxVectorValue::Emit(VMFunctionBuilder *build)
{
	// no const handling here. Ultimstely it's too rarely used (i.e. the only fully constant vector ever allocated in ZDoom is the 0-vector in a very few places)
	// and the negatives (excessive allocation of float constants) outweigh the positives (saved a few instructions)
	assert(xyz[0] != nullptr);
	assert(xyz[1] != nullptr);
	if (ValueType == TypeVector2)
	{
		ExpEmit tempxval = xyz[0]->Emit(build);
		ExpEmit tempyval = xyz[1]->Emit(build);
		ExpEmit xval = EmitKonst(build, tempxval);
		ExpEmit yval = EmitKonst(build, tempyval);
		assert(xval.RegType == REGT_FLOAT && yval.RegType == REGT_FLOAT);
		if (yval.RegNum == xval.RegNum + 1)
		{
			// The results are already in two continuous registers so just return them as-is.
			xval.RegCount++;
			return xval;
		}
		else
		{
			// The values are not in continuous registers so they need to be copied together now.
			ExpEmit out(build, REGT_FLOAT, 2);
			build->Emit(OP_MOVEF, out.RegNum, xval.RegNum);
			build->Emit(OP_MOVEF, out.RegNum + 1, yval.RegNum);
			xval.Free(build);
			yval.Free(build);
			return out;
		}
	}
	else if (xyz[0]->ValueType == TypeVector2)	// vec2+float
	{
		ExpEmit xyval = xyz[0]->Emit(build);
		ExpEmit tempzval = xyz[1]->Emit(build);
		ExpEmit zval = EmitKonst(build, tempzval);
		assert(xyval.RegType == REGT_FLOAT && xyval.RegCount == 2 && zval.RegType == REGT_FLOAT);
		if (zval.RegNum == xyval.RegNum + 2)
		{
			// The results are already in three continuous registers so just return them as-is.
			xyval.RegCount++;
			return xyval;
		}
		else
		{
			// The values are not in continuous registers so they need to be copied together now.
			ExpEmit out(build, REGT_FLOAT, 3);
			build->Emit(OP_MOVEV2, out.RegNum, xyval.RegNum);
			build->Emit(OP_MOVEF, out.RegNum + 2, zval.RegNum);
			xyval.Free(build);
			zval.Free(build);
			return out;
		}
	}
	else // 3*float
	{
		assert(xyz[2] != nullptr);
		ExpEmit tempxval = xyz[0]->Emit(build);
		ExpEmit tempyval = xyz[1]->Emit(build);
		ExpEmit tempzval = xyz[2]->Emit(build);
		ExpEmit xval = EmitKonst(build, tempxval);
		ExpEmit yval = EmitKonst(build, tempyval);
		ExpEmit zval = EmitKonst(build, tempzval);
		assert(xval.RegType == REGT_FLOAT && yval.RegType == REGT_FLOAT && zval.RegType == REGT_FLOAT);
		if (yval.RegNum == xval.RegNum + 1 && zval.RegNum == xval.RegNum + 2)
		{
			// The results are already in three continuous registers so just return them as-is.
			xval.RegCount += 2;
			return xval;
		}
		else
		{
			// The values are not in continuous registers so they need to be copied together now.
			ExpEmit out(build, REGT_FLOAT, 3);
			//Try to optimize a bit...
			if (yval.RegNum == xval.RegNum + 1)
			{
				build->Emit(OP_MOVEV2, out.RegNum, xval.RegNum);
				build->Emit(OP_MOVEF, out.RegNum + 2, zval.RegNum);
			}
			else if (zval.RegNum == yval.RegNum + 1)
			{
				build->Emit(OP_MOVEF, out.RegNum, xval.RegNum);
				build->Emit(OP_MOVEV2, out.RegNum+1, yval.RegNum);
			}
			else
			{
				build->Emit(OP_MOVEF, out.RegNum, xval.RegNum);
				build->Emit(OP_MOVEF, out.RegNum + 1, yval.RegNum);
				build->Emit(OP_MOVEF, out.RegNum + 2, zval.RegNum);
			}
			xval.Free(build);
			yval.Free(build);
			zval.Free(build);
			return out;
		}
	}
}

//==========================================================================
//
//
//
//==========================================================================

FxBoolCast::FxBoolCast(FxExpression *x, bool needvalue)
	: FxExpression(EFX_BoolCast, x->ScriptPosition)
{
	basex = x;
	ValueType = TypeBool;
	NeedValue = needvalue;
}

//==========================================================================
//
//
//
//==========================================================================

FxBoolCast::~FxBoolCast()
{
	SAFE_DELETE(basex);
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxBoolCast::Resolve(FCompileContext &ctx)
{
	CHECKRESOLVED();
	SAFE_RESOLVE(basex, ctx);

	if (basex->ValueType == TypeBool)
	{
		FxExpression *x = basex;
		basex = nullptr;
		delete this;
		return x;
	}
	else if (basex->IsBoolCompat())
	{
		if (basex->isConstant())
		{
			assert(basex->ValueType != TypeState && "We shouldn't be able to generate a constant state ref");

			ExpVal constval = static_cast<FxConstant *>(basex)->GetValue();
			FxExpression *x = new FxConstant(constval.GetBool(), ScriptPosition);
			delete this;
			return x;
		}
		return this;
	}
	ScriptPosition.Message(MSG_ERROR, "Numeric type expected");
	delete this;
	return nullptr;
}

//==========================================================================
//
//
//
//==========================================================================

ExpEmit FxBoolCast::Emit(VMFunctionBuilder *build)
{
	ExpEmit from = basex->Emit(build);
	assert(!from.Konst);
	assert(basex->ValueType->GetRegType() == REGT_INT || basex->ValueType->GetRegType() == REGT_FLOAT || basex->ValueType->GetRegType() == REGT_POINTER);

	if (NeedValue)
	{
		ExpEmit to(build, REGT_INT);
		from.Free(build);
		// Preload result with 0.
		build->Emit(OP_LI, to.RegNum, 0);

		// Check source against 0.
		if (from.RegType == REGT_INT)
		{
			build->Emit(OP_EQ_R, 1, from.RegNum, to.RegNum);
		}
		else if (from.RegType == REGT_FLOAT)
		{
			build->Emit(OP_EQF_K, 1, from.RegNum, build->GetConstantFloat(0.));
		}
		else if (from.RegType == REGT_POINTER)
		{
			build->Emit(OP_EQA_K, 1, from.RegNum, build->GetConstantAddress(nullptr, ATAG_GENERIC));
		}
		build->Emit(OP_JMP, 1);

		// Reload result with 1 if the comparison fell through.
		build->Emit(OP_LI, to.RegNum, 1);
		return to;
	}
	else
	{
		return from;
	}
}

//==========================================================================
//
//
//
//==========================================================================

FxIntCast::FxIntCast(FxExpression *x, bool nowarn, bool explicitly)
: FxExpression(EFX_IntCast, x->ScriptPosition)
{
	basex=x;
	ValueType = TypeSInt32;
	NoWarn = nowarn;
	Explicit = explicitly;
}

//==========================================================================
//
//
//
//==========================================================================

FxIntCast::~FxIntCast()
{
	SAFE_DELETE(basex);
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxIntCast::Resolve(FCompileContext &ctx)
{
	CHECKRESOLVED();
	SAFE_RESOLVE(basex, ctx);

	if (basex->ValueType->GetRegType() == REGT_INT)
	{
		if (basex->ValueType->isNumeric() || Explicit)	// names can be converted to int, but only with an explicit type cast.
		{
			FxExpression *x = basex;
			x->ValueType = ValueType;
			basex = nullptr;
			delete this;
			return x;
		}
		else
		{
			// Ugh. This should abort, but too many mods fell into this logic hole somewhere, so this serious error needs to be reduced to a warning. :(
			// At least in ZScript, MSG_OPTERROR always means to report an error, not a warning so the problem only exists in DECORATE.
			if (!basex->isConstant())	
				ScriptPosition.Message(MSG_OPTERROR, "Numeric type expected, got a name");
			else ScriptPosition.Message(MSG_OPTERROR, "Numeric type expected, got \"%s\"", static_cast<FxConstant*>(basex)->GetValue().GetName().GetChars());
			FxExpression * x = new FxConstant(0, ScriptPosition);
			delete this;
			return x;
		}
	}
	else if (basex->IsFloat())
	{
		if (basex->isConstant())
		{
			ExpVal constval = static_cast<FxConstant *>(basex)->GetValue();
			FxExpression *x = new FxConstant(constval.GetInt(), ScriptPosition);
			if (constval.GetInt() != constval.GetFloat())
			{
				ScriptPosition.Message(MSG_WARNING, "Truncation of floating point constant %f", constval.GetFloat());
			}

			delete this;
			return x;
		}
		else if (!NoWarn)
		{
			ScriptPosition.Message(MSG_DEBUGWARN, "Truncation of floating point value");
		}

		return this;
	}
	ScriptPosition.Message(MSG_ERROR, "Numeric type expected");
	delete this;
	return nullptr;
}

//==========================================================================
//
//
//
//==========================================================================

ExpEmit FxIntCast::Emit(VMFunctionBuilder *build)
{
	ExpEmit from = basex->Emit(build);
	assert(!from.Konst);
	assert(basex->ValueType->GetRegType() == REGT_FLOAT);
	from.Free(build);
	ExpEmit to(build, REGT_INT);
	build->Emit(OP_CAST, to.RegNum, from.RegNum, ValueType == TypeUInt32? CAST_F2U : CAST_F2I);
	return to;
}

//==========================================================================
//
//
//
//==========================================================================

FxFloatCast::FxFloatCast(FxExpression *x)
	: FxExpression(EFX_FloatCast, x->ScriptPosition)
{
	basex = x;
	ValueType = TypeFloat64;
}

//==========================================================================
//
//
//
//==========================================================================

FxFloatCast::~FxFloatCast()
{
	SAFE_DELETE(basex);
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxFloatCast::Resolve(FCompileContext &ctx)
{
	CHECKRESOLVED();
	SAFE_RESOLVE(basex, ctx);

	if (basex->IsFloat())
	{
		FxExpression *x = basex;
		basex = nullptr;
		delete this;
		return x;
	}
	else if (basex->ValueType->GetRegType() == REGT_INT)
	{
		if (basex->ValueType->isNumeric())
		{
			if (basex->isConstant())
			{
				ExpVal constval = static_cast<FxConstant *>(basex)->GetValue();
				FxExpression *x = new FxConstant(constval.GetFloat(), ScriptPosition);
				delete this;
				return x;
			}
			return this;
		}
		else
		{
			// Ugh. This should abort, but too many mods fell into this logic hole somewhere, so this seroious error needs to be reduced to a warning. :(
			// At least in ZScript, MSG_OPTERROR always means to report an error, not a warning so the problem only exists in DECORATE.
			if (!basex->isConstant()) ScriptPosition.Message(MSG_OPTERROR, "Numeric type expected, got a name");
			else ScriptPosition.Message(MSG_OPTERROR, "Numeric type expected, got \"%s\"", static_cast<FxConstant*>(basex)->GetValue().GetName().GetChars());
			FxExpression *x = new FxConstant(0.0, ScriptPosition);
			delete this;
			return x;
		}
	}
	else
	{
		ScriptPosition.Message(MSG_ERROR, "Numeric type expected");
		delete this;
		return nullptr;
	}
}

//==========================================================================
//
//
//
//==========================================================================

ExpEmit FxFloatCast::Emit(VMFunctionBuilder *build)
{
	ExpEmit from = basex->Emit(build);
	assert(!from.Konst);
	assert(basex->ValueType->GetRegType() == REGT_INT);
	from.Free(build);
	ExpEmit to(build, REGT_FLOAT);
	build->Emit(OP_CAST, to.RegNum, from.RegNum, basex->ValueType == TypeUInt32? CAST_U2F : CAST_I2F);
	return to;
}

//==========================================================================
//
//
//
//==========================================================================

FxNameCast::FxNameCast(FxExpression *x)
	: FxExpression(EFX_NameCast, x->ScriptPosition)
{
	basex = x;
	ValueType = TypeName;
}

//==========================================================================
//
//
//
//==========================================================================

FxNameCast::~FxNameCast()
{
	SAFE_DELETE(basex);
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxNameCast::Resolve(FCompileContext &ctx)
{
	CHECKRESOLVED();
	SAFE_RESOLVE(basex, ctx);

	if (basex->ValueType == TypeName)
	{
		FxExpression *x = basex;
		basex = nullptr;
		delete this;
		return x;
	}
	else if (basex->ValueType == TypeString)
	{
		if (basex->isConstant())
		{
			ExpVal constval = static_cast<FxConstant *>(basex)->GetValue();
			FxExpression *x = new FxConstant(constval.GetName(), ScriptPosition);
			delete this;
			return x;
		}
		return this;
	}
	else
	{
		ScriptPosition.Message(MSG_ERROR, "Cannot convert to name");
		delete this;
		return nullptr;
	}
}

//==========================================================================
//
//
//
//==========================================================================

ExpEmit FxNameCast::Emit(VMFunctionBuilder *build)
{
	ExpEmit from = basex->Emit(build);
	assert(!from.Konst);
	assert(basex->ValueType == TypeString);
	from.Free(build);
	ExpEmit to(build, REGT_INT);
	build->Emit(OP_CAST, to.RegNum, from.RegNum, CAST_S2N);
	return to;
}

//==========================================================================
//
//
//
//==========================================================================

FxStringCast::FxStringCast(FxExpression *x)
	: FxExpression(EFX_StringCast, x->ScriptPosition)
{
	basex = x;
	ValueType = TypeString;
}

//==========================================================================
//
//
//
//==========================================================================

FxStringCast::~FxStringCast()
{
	SAFE_DELETE(basex);
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxStringCast::Resolve(FCompileContext &ctx)
{
	CHECKRESOLVED();
	SAFE_RESOLVE(basex, ctx);

	if (basex->ValueType == TypeString)
	{
		FxExpression *x = basex;
		basex = nullptr;
		delete this;
		return x;
	}
	else if (basex->ValueType == TypeName)
	{
		if (basex->isConstant())
		{
			ExpVal constval = static_cast<FxConstant *>(basex)->GetValue();
			FxExpression *x = new FxConstant(constval.GetString(), ScriptPosition);
			delete this;
			return x;
		}
		return this;
	}
	else if (basex->ValueType == TypeSound)
	{
		if (basex->isConstant())
		{
			ExpVal constval = static_cast<FxConstant *>(basex)->GetValue();
			FxExpression *x = new FxConstant(S_sfx[constval.GetInt()].name, ScriptPosition);
			delete this;
			return x;
		}
		return this;
	}
	// although it could be done, let's not convert colors back to strings.
	else
	{
		ScriptPosition.Message(MSG_ERROR, "Cannot convert to string");
		delete this;
		return nullptr;
	}
}

//==========================================================================
//
//
//
//==========================================================================

ExpEmit FxStringCast::Emit(VMFunctionBuilder *build)
{
	ExpEmit from = basex->Emit(build);
	assert(!from.Konst);

	from.Free(build);
	ExpEmit to(build, REGT_STRING);
	if (basex->ValueType == TypeName)
	{
		build->Emit(OP_CAST, to.RegNum, from.RegNum, CAST_N2S);
	}
	else if (basex->ValueType == TypeSound)
	{
		build->Emit(OP_CAST, to.RegNum, from.RegNum, CAST_So2S);
	}
	return to;
}

//==========================================================================
//
//
//
//==========================================================================

FxColorCast::FxColorCast(FxExpression *x)
	: FxExpression(EFX_ColorCast, x->ScriptPosition)
{
	basex = x;
	ValueType = TypeColor;
}

//==========================================================================
//
//
//
//==========================================================================

FxColorCast::~FxColorCast()
{
	SAFE_DELETE(basex);
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxColorCast::Resolve(FCompileContext &ctx)
{
	CHECKRESOLVED();
	SAFE_RESOLVE(basex, ctx);

	if (basex->ValueType == TypeColor || basex->ValueType->GetClass() == RUNTIME_CLASS(PInt))
	{
		FxExpression *x = basex;
		x->ValueType = TypeColor;
		basex = nullptr;
		delete this;
		return x;
	}
	else if (basex->ValueType == TypeString)
	{
		if (basex->isConstant())
		{
			ExpVal constval = static_cast<FxConstant *>(basex)->GetValue();
			if (constval.GetString().Len() == 0)
			{
				// empty string means 'no state'. This would otherwise just cause endless errors and have the same result anyway.
				FxExpression *x = new FxConstant(-1, ScriptPosition);
				delete this;
				return x;
			}
			else
			{
				FxExpression *x = new FxConstant(V_GetColor(nullptr, constval.GetString()), ScriptPosition);
				delete this;
				return x;
			}
		}
		return this;
	}
	else
	{
		ScriptPosition.Message(MSG_ERROR, "Cannot convert to color");
		delete this;
		return nullptr;
	}
}

//==========================================================================
//
//
//
//==========================================================================

ExpEmit FxColorCast::Emit(VMFunctionBuilder *build)
{
	ExpEmit from = basex->Emit(build);
	assert(!from.Konst);
	assert(basex->ValueType == TypeString);
	from.Free(build);
	ExpEmit to(build, REGT_INT);
	build->Emit(OP_CAST, to.RegNum, from.RegNum, CAST_S2Co);
	return to;
}

//==========================================================================
//
//
//
//==========================================================================

FxSoundCast::FxSoundCast(FxExpression *x)
	: FxExpression(EFX_SoundCast, x->ScriptPosition)
{
	basex = x;
	ValueType = TypeSound;
}

//==========================================================================
//
//
//
//==========================================================================

FxSoundCast::~FxSoundCast()
{
	SAFE_DELETE(basex);
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxSoundCast::Resolve(FCompileContext &ctx)
{
	CHECKRESOLVED();
	SAFE_RESOLVE(basex, ctx);

	if (basex->ValueType == TypeSound || basex->ValueType->GetClass() == RUNTIME_CLASS(PInt))
	{
		FxExpression *x = basex;
		x->ValueType = TypeSound;
		basex = nullptr;
		delete this;
		return x;
	}
	else if (basex->ValueType == TypeString)
	{
		if (basex->isConstant())
		{
			ExpVal constval = static_cast<FxConstant *>(basex)->GetValue();
			FxExpression *x = new FxConstant(FSoundID(constval.GetString()), ScriptPosition);
			delete this;
			return x;
		}
		return this;
	}
	else
	{
		ScriptPosition.Message(MSG_ERROR, "Cannot convert to sound");
		delete this;
		return nullptr;
	}
}

//==========================================================================
//
//
//
//==========================================================================

ExpEmit FxSoundCast::Emit(VMFunctionBuilder *build)
{
	ExpEmit from = basex->Emit(build);
	assert(!from.Konst);
	assert(basex->ValueType == TypeString);
	from.Free(build);
	ExpEmit to(build, REGT_INT);
	build->Emit(OP_CAST, to.RegNum, from.RegNum, CAST_S2So);
	return to;
}

//==========================================================================
//
// generic type cast operator
//
//==========================================================================

FxTypeCast::FxTypeCast(FxExpression *x, PType *type, bool nowarn, bool explicitly)
	: FxExpression(EFX_TypeCast, x->ScriptPosition)
{
	basex = x;
	ValueType = type;
	NoWarn = nowarn;
	Explicit = explicitly;
	assert(ValueType != nullptr);
}

//==========================================================================
//
//
//
//==========================================================================

FxTypeCast::~FxTypeCast()
{
	SAFE_DELETE(basex);
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxTypeCast::Resolve(FCompileContext &ctx)
{
	CHECKRESOLVED();
	SAFE_RESOLVE(basex, ctx);

	// first deal with the simple types
	if (ValueType == TypeError || basex->ValueType == TypeError)
	{
		ScriptPosition.Message(MSG_ERROR, "Trying to cast to invalid type.");
		delete this;
		return nullptr;
	}
	else if (ValueType == TypeVoid)	// this should never happen
	{
		goto errormsg;
	}
	else if (basex->ValueType == TypeVoid)
	{
		goto errormsg;
	}
	else if (basex->ValueType == ValueType)
	{
		// don't go through the entire list if the types are the same.
		goto basereturn;
	}
	else if (basex->ValueType == TypeNullPtr && (ValueType == TypeState || ValueType->IsKindOf(RUNTIME_CLASS(PPointer))))
	{
		goto basereturn;
	}
	else if (IsFloat())
	{
		FxExpression *x = new FxFloatCast(basex);
		x = x->Resolve(ctx);
		basex = nullptr;
		delete this;
		return x;
	}
	else if (ValueType->IsA(RUNTIME_CLASS(PInt)))
	{
		// This is only for casting to actual ints. Subtypes representing an int will be handled elsewhere.
		FxExpression *x = new FxIntCast(basex, NoWarn, Explicit);
		x = x->Resolve(ctx);
		basex = nullptr;
		delete this;
		return x;
	}
	else if (ValueType == TypeBool)
	{
		FxExpression *x = new FxBoolCast(basex);
		x = x->Resolve(ctx);
		basex = nullptr;
		delete this;
		return x;
	}
	else if (ValueType == TypeString)
	{
		FxExpression *x = new FxStringCast(basex);
		x = x->Resolve(ctx);
		basex = nullptr;
		delete this;
		return x;
	}
	else if (ValueType == TypeName)
	{
		FxExpression *x = new FxNameCast(basex);
		x = x->Resolve(ctx);
		basex = nullptr;
		delete this;
		return x;
	}
	else if (ValueType == TypeSound)
	{
		FxExpression *x = new FxSoundCast(basex);
		x = x->Resolve(ctx);
		basex = nullptr;
		delete this;
		return x;
	}
	else if (ValueType == TypeColor)
	{
		FxExpression *x = new FxColorCast(basex);
		x = x->Resolve(ctx);
		basex = nullptr;
		delete this;
		return x;
	}
	else if (ValueType == TypeSpriteID && basex->IsInteger())
	{
		basex->ValueType = TypeSpriteID;
		auto x = basex;
		basex = nullptr;
		delete this;
		return x;
	}
	else if (ValueType == TypeStateLabel)
	{
		if (basex->ValueType == TypeNullPtr)
		{
			auto x = new FxConstant(0, ScriptPosition);
			x->ValueType = TypeStateLabel;
			delete this;
			return x;
		}
		// Right now this only supports string constants. There should be an option to pass a string variable, too.
		if (basex->isConstant() && (basex->ValueType == TypeString || basex->ValueType == TypeName))
		{
			FString s= static_cast<FxConstant *>(basex)->GetValue().GetString();
			if (s.Len() == 0 && !ctx.FromDecorate)	// DECORATE should never get here at all, but let's better be safe.
			{
				ScriptPosition.Message(MSG_ERROR, "State jump to empty label.");
				delete this;
				return nullptr;
			}
			FxExpression *x = new FxMultiNameState(s, basex->ScriptPosition);
			x = x->Resolve(ctx);
			basex = nullptr;
			delete this;
			return x;
		}
		else if (basex->IsNumeric() && basex->ValueType != TypeSound && basex->ValueType != TypeColor)
		{
			if (ctx.StateIndex < 0)
			{
				ScriptPosition.Message(MSG_ERROR, "State jumps with index can only be used in anonymous state functions.");
				delete this;
				return nullptr;
			}
			if (ctx.StateCount != 1)
			{
				ScriptPosition.Message(MSG_ERROR, "State jumps with index cannot be used on multistate definitions");
				delete this;
				return nullptr;
			}
			if (basex->isConstant())
			{
				int i = static_cast<FxConstant *>(basex)->GetValue().GetInt();
				if (i <= 0)
				{
					ScriptPosition.Message(MSG_ERROR, "State index must be positive");
					delete this;
					return nullptr;
				}
				FxExpression *x = new FxStateByIndex(ctx.StateIndex + i, ScriptPosition);
				x = x->Resolve(ctx);
				basex = nullptr;
				delete this;
				return x;
			}
			else
			{
				FxExpression *x = new FxRuntimeStateIndex(basex);
				x = x->Resolve(ctx);
				basex = nullptr;
				delete this;
				return x;
			}
		}
	}
	else if (ValueType->IsKindOf(RUNTIME_CLASS(PClassPointer)))
	{
		FxExpression *x = new FxClassTypeCast(static_cast<PClassPointer*>(ValueType), basex);
		x = x->Resolve(ctx);
		basex = nullptr;
		delete this;
		return x;
	}
	/* else if (ValueType->IsKindOf(RUNTIME_CLASS(PEnum)))
	{
	// this is not yet ready and does not get assigned to actual values.
	}
	*/
	else if (ValueType->IsKindOf(RUNTIME_CLASS(PClass)))	// this should never happen because the VM doesn't handle plain class types - just pointers
	{
		if (basex->ValueType->IsKindOf(RUNTIME_CLASS(PClass)))
		{
			// class types are only compatible if the base type is a descendant of the result type.
			auto fromtype = static_cast<PClass *>(basex->ValueType);
			auto totype = static_cast<PClass *>(ValueType);
			if (fromtype->IsDescendantOf(totype)) goto basereturn;
		}
	}
	else if (AreCompatiblePointerTypes(ValueType, basex->ValueType))
	{
		goto basereturn;
	}
	// todo: pointers to class objects. 
	// All other types are only compatible to themselves and have already been handled above by the equality check.
	// Anything that falls through here is not compatible and must print an error.

errormsg:
	ScriptPosition.Message(MSG_ERROR, "Cannot convert %s to %s", basex->ValueType->DescriptiveName(), ValueType->DescriptiveName());
	delete this;
	return nullptr;

basereturn:
	auto x = basex;
	x->ValueType = ValueType;
	basex = nullptr;
	delete this;
	return x;

}

//==========================================================================
//
//
//
//==========================================================================

ExpEmit FxTypeCast::Emit(VMFunctionBuilder *build)
{
	assert(false);
	// This should never be reached
	return ExpEmit();
}

//==========================================================================
//
//
//
//==========================================================================

FxPlusSign::FxPlusSign(FxExpression *operand)
: FxExpression(EFX_PlusSign, operand->ScriptPosition)
{
	Operand=operand;
}

//==========================================================================
//
//
//
//==========================================================================

FxPlusSign::~FxPlusSign()
{
	SAFE_DELETE(Operand);
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxPlusSign::Resolve(FCompileContext& ctx)
{
	CHECKRESOLVED();
	SAFE_RESOLVE(Operand, ctx);

	if (Operand->IsNumeric() || Operand->IsVector())
	{
		FxExpression *e = Operand;
		Operand = nullptr;
		delete this;
		return e;
	}
	else
	{
		ScriptPosition.Message(MSG_ERROR, "Numeric type expected");
		delete this;
		return nullptr;
	}
}

ExpEmit FxPlusSign::Emit(VMFunctionBuilder *build)
{
	return Operand->Emit(build);
}

//==========================================================================
//
//
//
//==========================================================================

FxMinusSign::FxMinusSign(FxExpression *operand)
: FxExpression(EFX_MinusSign, operand->ScriptPosition)
{
	Operand=operand;
}

//==========================================================================
//
//
//
//==========================================================================

FxMinusSign::~FxMinusSign()
{
	SAFE_DELETE(Operand);
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxMinusSign::Resolve(FCompileContext& ctx)
{
	CHECKRESOLVED();
	SAFE_RESOLVE(Operand, ctx);

	if (Operand->IsNumeric() || Operand->IsVector())
	{
		if (Operand->isConstant())
		{
			ExpVal val = static_cast<FxConstant *>(Operand)->GetValue();
			FxExpression *e = val.Type->GetRegType() == REGT_INT ?
				new FxConstant(-val.Int, ScriptPosition) :
				new FxConstant(-val.Float, ScriptPosition);
			delete this;
			return e;
		}
		ValueType = Operand->ValueType;
		return this;
	}
	else
	{
		ScriptPosition.Message(MSG_ERROR, "Numeric type expected");
		delete this;
		return nullptr;
	}
}

//==========================================================================
//
//
//
//==========================================================================

ExpEmit FxMinusSign::Emit(VMFunctionBuilder *build)
{
	assert(ValueType == Operand->ValueType);
	ExpEmit from = Operand->Emit(build);
	ExpEmit to;
	assert(from.Konst == 0);
	assert(ValueType->GetRegCount() == from.RegCount);
	// Do it in-place, unless a local variable
	if (from.Fixed)
	{
		to = ExpEmit(build, from.RegType, from.RegCount);
		from.Free(build);
	}
	else
	{
		to = from;
	}

	if (ValueType->GetRegType() == REGT_INT)
	{
		build->Emit(OP_NEG, to.RegNum, from.RegNum, 0);
	}
	else
	{
		assert(ValueType->GetRegType() == REGT_FLOAT);
		switch (from.RegCount)
		{
		case 1:
			build->Emit(OP_FLOP, to.RegNum, from.RegNum, FLOP_NEG);
			break;

		case 2:
			build->Emit(OP_NEGV2, to.RegNum, from.RegNum);
			break;

		case 3:
			build->Emit(OP_NEGV3, to.RegNum, from.RegNum);
			break;

		}
	}
	return to;
}

//==========================================================================
//
//
//
//==========================================================================

FxUnaryNotBitwise::FxUnaryNotBitwise(FxExpression *operand)
: FxExpression(EFX_UnaryNotBitwise, operand->ScriptPosition)
{
	Operand=operand;
}

//==========================================================================
//
//
//
//==========================================================================

FxUnaryNotBitwise::~FxUnaryNotBitwise()
{
	SAFE_DELETE(Operand);
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxUnaryNotBitwise::Resolve(FCompileContext& ctx)
{
	CHECKRESOLVED();
	SAFE_RESOLVE(Operand, ctx);

	if  (ctx.FromDecorate && Operand->IsFloat() /* lax */)
	{
		// DECORATE allows floats here so cast them to int.
		Operand = new FxIntCast(Operand, true);
		Operand = Operand->Resolve(ctx);
		if (Operand == nullptr) 
		{
			delete this;
			return nullptr;
		}
	}

	// Names were not blocked in DECORATE here after the scripting branch merge. Now they are again.
	if (!Operand->IsInteger())
	{
		ScriptPosition.Message(MSG_ERROR, "Integer type expected");
		delete this;
		return nullptr;
	}

	if (Operand->isConstant())
	{
		int result = ~static_cast<FxConstant *>(Operand)->GetValue().GetInt();
		FxExpression *e = new FxConstant(result, ScriptPosition);
		delete this;
		return e;
	}
	ValueType = TypeSInt32;
	return this;
}

//==========================================================================
//
//
//
//==========================================================================

ExpEmit FxUnaryNotBitwise::Emit(VMFunctionBuilder *build)
{
	assert(Operand->ValueType->GetRegType() == REGT_INT);
	ExpEmit from = Operand->Emit(build);
	assert(!from.Konst);
	// Do it in-place.
	build->Emit(OP_NOT, from.RegNum, from.RegNum, 0);
	return from;
}

//==========================================================================
//
//
//
//==========================================================================

FxUnaryNotBoolean::FxUnaryNotBoolean(FxExpression *operand)
: FxExpression(EFX_UnaryNotBoolean, operand->ScriptPosition)
{
	Operand=operand;
}

//==========================================================================
//
//
//
//==========================================================================

FxUnaryNotBoolean::~FxUnaryNotBoolean()
{
	SAFE_DELETE(Operand);
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxUnaryNotBoolean::Resolve(FCompileContext& ctx)
{
	CHECKRESOLVED();
	SAFE_RESOLVE(Operand, ctx);

	if (Operand->ValueType != TypeBool)
	{
		Operand = new FxBoolCast(Operand);
		SAFE_RESOLVE(Operand, ctx);
	}

	if (Operand->isConstant())
	{
		bool result = !static_cast<FxConstant *>(Operand)->GetValue().GetBool();
		FxExpression *e = new FxConstant(result, ScriptPosition);
		delete this;
		return e;
	}

	ValueType = TypeBool;
	return this;
}

//==========================================================================
//
//
//
//==========================================================================

ExpEmit FxUnaryNotBoolean::Emit(VMFunctionBuilder *build)
{
	assert(Operand->ValueType == TypeBool);
	assert(ValueType == TypeBool || IsInteger());	// this may have been changed by an int cast.
	ExpEmit from = Operand->Emit(build);
	from.Free(build);
	ExpEmit to(build, REGT_INT);
	assert(!from.Konst);
	// boolean not is the same as XOR-ing the lowest bit

	build->Emit(OP_XOR_RK, to.RegNum, from.RegNum, build->GetConstantInt(1));
	return to;
}

//==========================================================================
//
//
//
//==========================================================================

FxSizeAlign::FxSizeAlign(FxExpression *operand, int which)
	: FxExpression(EFX_SizeAlign, operand->ScriptPosition)
{
	Operand = operand;
	Which = which;
}

//==========================================================================
//
//
//
//==========================================================================

FxSizeAlign::~FxSizeAlign()
{
	SAFE_DELETE(Operand);
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxSizeAlign::Resolve(FCompileContext& ctx)
{
	CHECKRESOLVED();
	SAFE_RESOLVE(Operand, ctx);

	auto type = Operand->ValueType;
	if (Operand->isConstant())
	{
		ScriptPosition.Message(MSG_ERROR, "cannot determine %s of a constant", Which == TK_AlignOf? "alignment" : "size");
		delete this;
		return nullptr;
	}
	else if (!Operand->RequestAddress(ctx, nullptr))
	{
		ScriptPosition.Message(MSG_ERROR, "Operand must be addressable to determine %s", Which == TK_AlignOf ? "alignment" : "size");
		delete this;
		return nullptr;
	}
	else
	{
		FxExpression *x = new FxConstant(Which == TK_AlignOf ? int(type->Align) : int(type->Size), Operand->ScriptPosition);
		delete this;
		return x->Resolve(ctx);
	}
}

ExpEmit FxSizeAlign::Emit(VMFunctionBuilder *build)
{
	return ExpEmit();
}

//==========================================================================
//
// FxPreIncrDecr
//
//==========================================================================

FxPreIncrDecr::FxPreIncrDecr(FxExpression *base, int token)
: FxExpression(EFX_PreIncrDecr, base->ScriptPosition), Token(token), Base(base)
{
	AddressRequested = false;
	AddressWritable = false;
}

FxPreIncrDecr::~FxPreIncrDecr()
{
	SAFE_DELETE(Base);
}

bool FxPreIncrDecr::RequestAddress(FCompileContext &ctx, bool *writable)
{
	AddressRequested = true;
	if (writable != nullptr) *writable = AddressWritable;
	return true;
}

FxExpression *FxPreIncrDecr::Resolve(FCompileContext &ctx)
{
	CHECKRESOLVED();
	SAFE_RESOLVE(Base, ctx);

	ValueType = Base->ValueType;

	if (!Base->IsNumeric())
	{
		ScriptPosition.Message(MSG_ERROR, "Numeric type expected");
		delete this;
		return nullptr;
	}
	else if (Base->ValueType == TypeBool)
	{
		ScriptPosition.Message(MSG_ERROR, "%s is not allowed on type bool", FScanner::TokenName(Token).GetChars());
		delete this;
		return nullptr;
	}
	if (!Base->RequestAddress(ctx, &AddressWritable) || !AddressWritable )
	{
		ScriptPosition.Message(MSG_ERROR, "Expression must be a modifiable value");
		delete this;
		return nullptr;
	}

	return this;
}

ExpEmit FxPreIncrDecr::Emit(VMFunctionBuilder *build)
{
	assert(Token == TK_Incr || Token == TK_Decr);
	assert(ValueType == Base->ValueType && IsNumeric());

	int zero = build->GetConstantInt(0);
	int regtype = ValueType->GetRegType();
	ExpEmit pointer = Base->Emit(build);
	ExpEmit value = pointer;

	if (!pointer.Target)
	{
		value = ExpEmit(build, regtype);
		build->Emit(ValueType->GetLoadOp(), value.RegNum, pointer.RegNum, zero);
	}

	if (regtype == REGT_INT)
	{
		build->Emit(OP_ADDI, value.RegNum, value.RegNum, uint8_t((Token == TK_Incr) ? 1 : -1));
	}
	else
	{
		build->Emit((Token == TK_Incr) ? OP_ADDF_RK : OP_SUBF_RK, value.RegNum, value.RegNum, build->GetConstantFloat(1.));
	}

	if (!pointer.Target)
	{
		build->Emit(ValueType->GetStoreOp(), pointer.RegNum, value.RegNum, zero);
	}

	if (AddressRequested)
	{
		value.Free(build);
		return pointer;
	}

	pointer.Free(build);
	return value;
}

//==========================================================================
//
// FxPostIncrDecr
//
//==========================================================================

FxPostIncrDecr::FxPostIncrDecr(FxExpression *base, int token)
: FxExpression(EFX_PostIncrDecr, base->ScriptPosition), Token(token), Base(base)
{
}

FxPostIncrDecr::~FxPostIncrDecr()
{
	SAFE_DELETE(Base);
}

FxExpression *FxPostIncrDecr::Resolve(FCompileContext &ctx)
{
	CHECKRESOLVED();
	SAFE_RESOLVE(Base, ctx);
	bool AddressWritable;

	ValueType = Base->ValueType;

	if (!Base->IsNumeric())
	{
		ScriptPosition.Message(MSG_ERROR, "Numeric type expected");
		delete this;
		return nullptr;
	}
	else if (Base->ValueType == TypeBool)
	{
		ScriptPosition.Message(MSG_ERROR, "%s is not allowed on type bool", FScanner::TokenName(Token).GetChars());
		delete this;
		return nullptr;
	}
	if (!Base->RequestAddress(ctx, &AddressWritable) || !AddressWritable)
	{
		ScriptPosition.Message(MSG_ERROR, "Expression must be a modifiable value");
		delete this;
		return nullptr;
	}

	return this;
}

ExpEmit FxPostIncrDecr::Emit(VMFunctionBuilder *build)
{
	assert(Token == TK_Incr || Token == TK_Decr);
	assert(ValueType == Base->ValueType && IsNumeric());

	int zero = build->GetConstantInt(0);
	int regtype = ValueType->GetRegType();
	ExpEmit pointer = Base->Emit(build);

	if (!pointer.Target)
	{
		ExpEmit out(build, regtype);
		build->Emit(ValueType->GetLoadOp(), out.RegNum, pointer.RegNum, zero);
		ExpEmit assign(build, regtype);
		if (regtype == REGT_INT)
		{
			build->Emit(OP_ADDI, assign.RegNum, out.RegNum, uint8_t((Token == TK_Incr) ? 1 : -1));
		}
		else
		{
			build->Emit((Token == TK_Incr) ? OP_ADDF_RK : OP_SUBF_RK, assign.RegNum, out.RegNum, build->GetConstantFloat(1.));
		}
		build->Emit(ValueType->GetStoreOp(), pointer.RegNum, assign.RegNum, zero);
		pointer.Free(build);
		assign.Free(build);
		return out;
	}
	else if (NeedResult)
	{
		ExpEmit out(build, regtype);
		if (regtype == REGT_INT)
		{
			build->Emit(OP_MOVE, out.RegNum, pointer.RegNum);
			build->Emit(OP_ADDI, pointer.RegNum, pointer.RegNum, uint8_t((Token == TK_Incr) ? 1 : -1));
		}
		else
		{
			build->Emit(OP_MOVEF, out.RegNum, pointer.RegNum);
			build->Emit((Token == TK_Incr) ? OP_ADDF_RK : OP_SUBF_RK, pointer.RegNum, pointer.RegNum, build->GetConstantFloat(1.));
		}
		pointer.Free(build);
		return out;
	}
	else
	{
		if (regtype == REGT_INT)
		{
			build->Emit(OP_ADDI, pointer.RegNum, pointer.RegNum, uint8_t((Token == TK_Incr) ? 1 : -1));
		}
		else
		{
			build->Emit((Token == TK_Incr) ? OP_ADDF_RK : OP_SUBF_RK, pointer.RegNum, pointer.RegNum, build->GetConstantFloat(1.));
		}
		pointer.Free(build);
		return ExpEmit();
	}
}

//==========================================================================
//
// FxAssign
//
//==========================================================================

FxAssign::FxAssign(FxExpression *base, FxExpression *right, bool ismodify)
: FxExpression(EFX_Assign, base->ScriptPosition), Base(base), Right(right), IsBitWrite(-1), IsModifyAssign(ismodify)
{
	AddressRequested = false;
	AddressWritable = false;
}

FxAssign::~FxAssign()
{
	SAFE_DELETE(Base);
	SAFE_DELETE(Right);
}

/* I don't think we should allow constructs like (a = b) = c;...
bool FxAssign::RequestAddress(FCompileContext &ctx, bool *writable)
{
	AddressRequested = true;
	if (writable != nullptr) *writable = AddressWritable;
	return true;
}
*/

FxExpression *FxAssign::Resolve(FCompileContext &ctx)
{
	CHECKRESOLVED();
	SAFE_RESOLVE(Base, ctx);

	ValueType = Base->ValueType;

	SAFE_RESOLVE(Right, ctx);

	if (IsModifyAssign && Base->ValueType == TypeBool && Right->ValueType != TypeBool)
	{
		// If the modify operation resulted in a type promotion from bool to int, this must be blocked.
		// (this means, for bool, only &=, ^= and |= are allowed, although DECORATE is more lax.)
		ScriptPosition.Message(MSG_ERROR, "Invalid modify/assign operation with a boolean operand");
		delete this;
		return nullptr;
	}

	// keep the redundant handling for numeric types here to avoid problems with DECORATE.
	// for non-numerics FxTypeCast can be used without issues.
	if (Base->IsNumeric() && Right->IsNumeric())
	{
		if (Right->ValueType != ValueType)
		{
			if (ValueType == TypeBool)
			{
				Right = new FxBoolCast(Right);
			}
			else if (ValueType->GetRegType() == REGT_INT)
			{
				Right = new FxIntCast(Right, ctx.FromDecorate);
			}
			else
			{
				Right = new FxFloatCast(Right);
			}
			SAFE_RESOLVE(Right, ctx);
		}
	}
	else if (Base->ValueType == Right->ValueType)
	{
		if (Base->ValueType->IsKindOf(RUNTIME_CLASS(PArray)))
		{
			ScriptPosition.Message(MSG_ERROR, "Cannot assign arrays");
			delete this;
			return nullptr;
		}
		if (!Base->IsVector() && Base->ValueType->IsKindOf(RUNTIME_CLASS(PStruct)))
		{
			ScriptPosition.Message(MSG_ERROR, "Struct assignment not implemented yet");
			delete this;
			return nullptr;
		}
		// Both types are the same so this is ok.
	}
	else if (Right->ValueType->IsA(RUNTIME_CLASS(PNativeStruct)) && Base->ValueType->IsKindOf(RUNTIME_CLASS(PPointer)) && static_cast<PPointer*>(Base->ValueType)->PointedType == Right->ValueType)
	{
		// allow conversion of native structs to pointers of the same type. This is necessary to assign elements from global arrays like players, sectors, etc. to local pointers.
		// For all other types this is not needed. Structs are not assignable and classes can only exist as references.
		bool writable;
		Right->RequestAddress(ctx, &writable);
		Right->ValueType = Base->ValueType;
	}
	else
	{
		// pass it to FxTypeCast for complete handling.
		Right = new FxTypeCast(Right, Base->ValueType, false);
		SAFE_RESOLVE(Right, ctx);
	}

	if (!Base->RequestAddress(ctx, &AddressWritable) || !AddressWritable)
	{
		ScriptPosition.Message(MSG_ERROR, "Expression must be a modifiable value");
		delete this;
		return nullptr;
	}

	// Special case: Assignment to a bitfield.
	IsBitWrite = Base->GetBitValue();
	return this;
}

ExpEmit FxAssign::Emit(VMFunctionBuilder *build)
{
	static const BYTE loadops[] = { OP_LK, OP_LKF, OP_LKS, OP_LKP };
	assert(ValueType == Base->ValueType);
	assert(ValueType->GetRegType() == Right->ValueType->GetRegType());

	ExpEmit pointer = Base->Emit(build);
	Address = pointer;

	ExpEmit result = Right->Emit(build);
	assert(result.RegType <= REGT_TYPE);

	if (pointer.Target)
	{
		if (result.Konst)
		{
			build->Emit(loadops[result.RegType], pointer.RegNum, result.RegNum);
		}
		else
		{
			build->Emit(Right->ValueType->GetMoveOp(), pointer.RegNum, result.RegNum);
		}
	}
	else
	{
		if (result.Konst)
		{
			ExpEmit temp(build, result.RegType);
			build->Emit(loadops[result.RegType], temp.RegNum, result.RegNum);
			result.Free(build);
			result = temp;
		}

		if (IsBitWrite == -1)
		{
			build->Emit(ValueType->GetStoreOp(), pointer.RegNum, result.RegNum, build->GetConstantInt(0));
		}
		else
		{
			build->Emit(OP_SBIT, pointer.RegNum, result.RegNum, 1 << IsBitWrite);
		}

	}

	if (AddressRequested)
	{
		result.Free(build);
		return pointer;
	}

	pointer.Free(build);
	return result;
}

//==========================================================================
//
//	FxAssignSelf
//
//==========================================================================

FxAssignSelf::FxAssignSelf(const FScriptPosition &pos)
: FxExpression(EFX_AssignSelf, pos)
{
	Assignment = nullptr;
}

FxExpression *FxAssignSelf::Resolve(FCompileContext &ctx)
{
	CHECKRESOLVED();

	// This should never happen if FxAssignSelf is used correctly
	assert(Assignment != nullptr);

	ValueType = Assignment->ValueType;

	return this;
}

ExpEmit FxAssignSelf::Emit(VMFunctionBuilder *build)
{
	assert(ValueType == Assignment->ValueType);
	ExpEmit pointer = Assignment->Address; // FxAssign should have already emitted it
	if (!pointer.Target)
	{
		ExpEmit out(build, ValueType->GetRegType(), ValueType->GetRegCount());
		if (Assignment->IsBitWrite != -1)
		{
			build->Emit(OP_LBIT, out.RegNum, pointer.RegNum, 1 << Assignment->IsBitWrite);
		}
		else
		{
			build->Emit(ValueType->GetLoadOp(), out.RegNum, pointer.RegNum, build->GetConstantInt(0));
		}
		return out;
	}
	else
	{
		return pointer;
	}
}


//==========================================================================
//
//
//
//==========================================================================

FxMultiAssign::FxMultiAssign(FArgumentList &base, FxExpression *right, const FScriptPosition &pos)
	:FxExpression(EFX_MultiAssign, pos)
{
	Base = std::move(base);
	Right = right;
	LocalVarContainer = new FxCompoundStatement(ScriptPosition);
}

//==========================================================================
//
//
//
//==========================================================================

FxMultiAssign::~FxMultiAssign()
{
	SAFE_DELETE(Right);
	SAFE_DELETE(LocalVarContainer);
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxMultiAssign::Resolve(FCompileContext &ctx)
{
	CHECKRESOLVED();
	SAFE_RESOLVE(Right, ctx);
	if (Right->ExprType != EFX_VMFunctionCall)
	{
		Right->ScriptPosition.Message(MSG_ERROR, "Function call expected on right side of multi-assigment");
		delete this;
		return nullptr;
	}
	auto VMRight = static_cast<FxVMFunctionCall *>(Right);
	auto rets = VMRight->GetReturnTypes();
	if (rets.Size() < Base.Size())
	{
		Right->ScriptPosition.Message(MSG_ERROR, "Insufficient returns in function %s", VMRight->Function->SymbolName.GetChars());
		delete this;
		return nullptr;
	}
	// Pack the generated data (temp local variables for the results and necessary type casts and single assignments) into a compound statement for easier management.
	for (unsigned i = 0; i < Base.Size(); i++)
	{
		auto singlevar = new FxLocalVariableDeclaration(rets[i], NAME_None, nullptr, 0, ScriptPosition);
		LocalVarContainer->Add(singlevar);
		Base[i] = Base[i]->Resolve(ctx);
		ABORT(Base[i]);
		auto varaccess = new FxLocalVariable(singlevar, ScriptPosition);
		auto assignee = new FxTypeCast(varaccess, Base[i]->ValueType, false);
		LocalVarContainer->Add(new FxAssign(Base[i], assignee, false));
	}
	auto x = LocalVarContainer->Resolve(ctx);
	LocalVarContainer = nullptr;
	ABORT(x);
	LocalVarContainer = static_cast<FxCompoundStatement*>(x);
	static_cast<FxVMFunctionCall *>(Right)->AssignCount = Base.Size();
	ValueType = TypeVoid;
	return this;
}

//==========================================================================
//
//
//
//==========================================================================

ExpEmit FxMultiAssign::Emit(VMFunctionBuilder *build)
{
	Right->Emit(build);
	for (unsigned i = 0; i < Base.Size(); i++)
	{
		LocalVarContainer->LocalVars[i]->SetReg(static_cast<FxVMFunctionCall *>(Right)->ReturnRegs[i]);
	}
	static_cast<FxVMFunctionCall *>(Right)->ReturnRegs.Clear();
	static_cast<FxVMFunctionCall *>(Right)->ReturnRegs.ShrinkToFit();
	return LocalVarContainer->Emit(build);
}

//==========================================================================
//
//
//
//==========================================================================

FxBinary::FxBinary(int o, FxExpression *l, FxExpression *r)
: FxExpression(EFX_Binary, l->ScriptPosition)
{
	Operator=o;
	left=l;
	right=r;
}

//==========================================================================
//
//
//
//==========================================================================

FxBinary::~FxBinary()
{
	SAFE_DELETE(left);
	SAFE_DELETE(right);
}

//==========================================================================
//
//
//
//==========================================================================

bool FxBinary::Promote(FCompileContext &ctx, bool forceint)
{
	// math operations of unsigned ints results in an unsigned int. (16 and 8 bit values never get here, they get promoted to regular ints elsewhere already.)
	if (left->ValueType == TypeUInt32 && right->ValueType == TypeUInt32)
	{
		ValueType = TypeUInt32;
	}
	else if (left->IsInteger() && right->IsInteger())
	{
		ValueType = TypeSInt32;		// Addition and subtraction forces all integer-derived types to signed int.
	}
	else if (!forceint)
	{
		ValueType = TypeFloat64;
		if (left->IsFloat() && right->IsInteger())
		{
			right = (new FxFloatCast(right))->Resolve(ctx);
		}
		else if (left->IsInteger() && right->IsFloat())
		{
			left = (new FxFloatCast(left))->Resolve(ctx);
		}
	}
	else if (ctx.FromDecorate)
	{
		// For DECORATE which allows floats here. ZScript does not.
		if (left->IsFloat())
		{
			left = new FxIntCast(left, ctx.FromDecorate);
			left = left->Resolve(ctx);
		}
		if (right->IsFloat())
		{
			right = new FxIntCast(right, ctx.FromDecorate);
			right = right->Resolve(ctx);
		}
		if (left == nullptr || right == nullptr)
		{
			delete this;
			return false;
		}
		ValueType = TypeSInt32;

	}
	else
	{
		ScriptPosition.Message(MSG_ERROR, "Integer operand expected");
		delete this;
		return false;
	}
	return true;
}

//==========================================================================
//
//
//
//==========================================================================

FxAddSub::FxAddSub(int o, FxExpression *l, FxExpression *r)
: FxBinary(o, l, r)
{
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxAddSub::Resolve(FCompileContext& ctx)
{
	CHECKRESOLVED();

	RESOLVE(left, ctx);
	RESOLVE(right, ctx);
	if (!left || !right)
	{
		delete this;
		return nullptr;
	}

	if (left->ValueType == TypeState && right->IsInteger() && Operator == '+' && !left->isConstant())
	{
		// This is the only special case of pointer addition that will be accepted - because it is used quite often in the existing game code.
		ValueType = TypeState;
		right = new FxMulDiv('*', right, new FxConstant((int)sizeof(FState), ScriptPosition));	// multiply by size here, so that constants can be better optimized.
		right = right->Resolve(ctx);
		ABORT(right);
	}
	else if (left->IsVector() && right->IsVector())
	{
		// a vector2 can be added to or subtracted from a vector 3 but it needs to be the right operand.
		if (left->ValueType == right->ValueType || (left->ValueType == TypeVector3 && right->ValueType == TypeVector2))
		{
			ValueType = left->ValueType;
		}
		else
		{
			goto error;
		}
	}
	else if (left->IsNumeric() && right->IsNumeric())
	{
		Promote(ctx);
	}
	else
	{
		// To check: It may be that this could pass in DECORATE, although setting TypeVoid here would pretty much prevent that.
		goto error;
	}

	if (left->isConstant() && right->isConstant())
	{
		if (IsFloat())
		{
			double v;
			double v1 = static_cast<FxConstant *>(left)->GetValue().GetFloat();
			double v2 = static_cast<FxConstant *>(right)->GetValue().GetFloat();

			v =	Operator == '+'? v1 + v2 : 
				Operator == '-'? v1 - v2 : 0;

			FxExpression *e = new FxConstant(v, ScriptPosition);
			delete this;
			return e;
		}
		else
		{
			int v;
			int v1 = static_cast<FxConstant *>(left)->GetValue().GetInt();
			int v2 = static_cast<FxConstant *>(right)->GetValue().GetInt();

			v =	Operator == '+'? v1 + v2 : 
				Operator == '-'? v1 - v2 : 0;

			FxExpression *e = new FxConstant(v, ScriptPosition);
			delete this;
			return e;

		}
	}
	return this;

error:
	ScriptPosition.Message(MSG_ERROR, "Incompatible operands for %s", Operator == '+' ? "addition" : "subtraction");
	delete this;
	return nullptr;
}

//==========================================================================
//
//
//
//==========================================================================

ExpEmit FxAddSub::Emit(VMFunctionBuilder *build)
{
	assert(Operator == '+' || Operator == '-');
	ExpEmit op1 = left->Emit(build);
	ExpEmit op2 = right->Emit(build);
	if (Operator == '+')
	{
		if (op1.RegType == REGT_POINTER)
		{
			assert(!op1.Konst);
			assert(op2.RegType == REGT_INT);
			op1.Free(build);
			op2.Free(build);
			ExpEmit opout(build, REGT_POINTER);
			build->Emit(op2.Konst? OP_ADDA_RK : OP_ADDA_RR, opout.RegNum, op1.RegNum, op2.RegNum);
			return opout;
		}
		// Since addition is commutative, only the second operand may be a constant.
		if (op1.Konst)
		{
			swapvalues(op1, op2);
		}
		assert(!op1.Konst);
		op1.Free(build);
		op2.Free(build);
		ExpEmit to(build, ValueType->GetRegType(), ValueType->GetRegCount());
		if (IsVector())
		{
			assert(op1.RegType == REGT_FLOAT && op2.RegType == REGT_FLOAT);
			build->Emit(right->ValueType == TypeVector2? OP_ADDV2_RR : OP_ADDV3_RR, to.RegNum, op1.RegNum, op2.RegNum);
			if (left->ValueType == TypeVector3 && right->ValueType == TypeVector2 && to.RegNum != op1.RegNum)
			{
				// must move the z-coordinate
				build->Emit(OP_MOVEF, to.RegNum + 2, op1.RegNum + 2);
			}
			return to;
		}
		else if (ValueType->GetRegType() == REGT_FLOAT)
		{
			assert(op1.RegType == REGT_FLOAT && op2.RegType == REGT_FLOAT);
			build->Emit(op2.Konst ? OP_ADDF_RK : OP_ADDF_RR, to.RegNum, op1.RegNum, op2.RegNum);
			return to;
		}
		else
		{
			assert(ValueType->GetRegType() == REGT_INT);
			assert(op1.RegType == REGT_INT && op2.RegType == REGT_INT);
			build->Emit(op2.Konst ? OP_ADD_RK : OP_ADD_RR, to.RegNum, op1.RegNum, op2.RegNum);
			return to;
		}
	}
	else
	{
		// Subtraction is not commutative, so either side may be constant (but not both).
		assert(!op1.Konst || !op2.Konst);
		op1.Free(build);
		op2.Free(build);
		ExpEmit to(build, ValueType->GetRegType(), ValueType->GetRegCount());
		if (IsVector())
		{
			assert(op1.RegType == REGT_FLOAT && op2.RegType == REGT_FLOAT);
			build->Emit(right->ValueType == TypeVector2 ? OP_SUBV2_RR : OP_SUBV3_RR, to.RegNum, op1.RegNum, op2.RegNum);
			return to;
		}
		else if (ValueType->GetRegType() == REGT_FLOAT)
		{
			assert(op1.RegType == REGT_FLOAT && op2.RegType == REGT_FLOAT);
			build->Emit(op1.Konst ? OP_SUBF_KR : op2.Konst ? OP_SUBF_RK : OP_SUBF_RR, to.RegNum, op1.RegNum, op2.RegNum);
			return to;
		}
		else
		{
			assert(ValueType->GetRegType() == REGT_INT);
			assert(op1.RegType == REGT_INT && op2.RegType == REGT_INT);
			build->Emit(op1.Konst ? OP_SUB_KR : op2.Konst ? OP_SUB_RK : OP_SUB_RR, to.RegNum, op1.RegNum, op2.RegNum);
			return to;
		}
	}
}

//==========================================================================
//
//
//
//==========================================================================

FxMulDiv::FxMulDiv(int o, FxExpression *l, FxExpression *r)
: FxBinary(o, l, r)
{
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxMulDiv::Resolve(FCompileContext& ctx)
{
	CHECKRESOLVED();

	RESOLVE(left, ctx);
	RESOLVE(right, ctx);
	if (!left || !right)
	{
		delete this;
		return nullptr;
	}

	if (left->IsVector() || right->IsVector())
	{
		switch (Operator)
		{
		case '/':
			// For division, the vector must be the first operand.
			if (right->IsVector()) goto error;

		case '*':
			if (left->IsVector() && right->IsNumeric())
			{
				if (right->IsInteger())
				{
					right = new FxFloatCast(right);
					right = right->Resolve(ctx);
					if (right == nullptr)
					{
						delete this;
						return nullptr;
					}
				}
				ValueType = left->ValueType;
			}
			else if (right->IsVector() && left->IsNumeric())
			{
				if (left->IsInteger())
				{
					left = new FxFloatCast(left);
					left = left->Resolve(ctx);
					if (left == nullptr)
					{
						delete this;
						return nullptr;
					}
				}
				ValueType = right->ValueType;
			}
			break;

		default:
			// Vector modulus is not permitted
			goto error;

		}
	}
	else if (left->IsNumeric() && right->IsNumeric())
	{
		Promote(ctx);
	}
	else
	{
		// To check: It may be that this could pass in DECORATE, although setting TypeVoid here would pretty much prevent that.
		goto error;
	}

	if (left->isConstant() && right->isConstant())
	{
		if (IsFloat())
		{
			double v;
			double v1 = static_cast<FxConstant *>(left)->GetValue().GetFloat();
			double v2 = static_cast<FxConstant *>(right)->GetValue().GetFloat();

			if (Operator != '*' && v2 == 0)
			{
				ScriptPosition.Message(MSG_ERROR, "Division by 0");
				delete this;
				return nullptr;
			}

			v =	Operator == '*'? v1 * v2 : 
				Operator == '/'? v1 / v2 : 
				Operator == '%'? fmod(v1, v2) : 0;

			FxExpression *e = new FxConstant(v, ScriptPosition);
			delete this;
			return e;
		}
		else
		{
			int v;
			int v1 = static_cast<FxConstant *>(left)->GetValue().GetInt();
			int v2 = static_cast<FxConstant *>(right)->GetValue().GetInt();

			if (Operator != '*' && v2 == 0)
			{
				ScriptPosition.Message(MSG_ERROR, "Division by 0");
				delete this;
				return nullptr;
			}

			v =	Operator == '*'? v1 * v2 : 
				Operator == '/'? v1 / v2 : 
				Operator == '%'? v1 % v2 : 0;

			FxExpression *e = new FxConstant(v, ScriptPosition);
			delete this;
			return e;

		}
	}
	return this;

error:
	ScriptPosition.Message(MSG_ERROR, "Incompatible operands for %s", Operator == '*' ? "multiplication" : Operator == '%' ? "modulus" : "division");
	delete this;
	return nullptr;

}


//==========================================================================
//
//
//
//==========================================================================

ExpEmit FxMulDiv::Emit(VMFunctionBuilder *build)
{
	ExpEmit op1 = left->Emit(build);
	ExpEmit op2 = right->Emit(build);

	if (IsVector())
	{
		assert(Operator != '%');
		if (right->IsVector())
		{
			swapvalues(op1, op2);
		}
		int op;
		if (op2.Konst)
		{
			op = Operator == '*' ? (ValueType == TypeVector2 ? OP_MULVF2_RK : OP_MULVF3_RK) : (ValueType == TypeVector2 ? OP_DIVVF2_RK : OP_DIVVF3_RK);
		}
		else
		{
			op = Operator == '*' ? (ValueType == TypeVector2 ? OP_MULVF2_RR : OP_MULVF3_RR) : (ValueType == TypeVector2 ? OP_DIVVF2_RR : OP_DIVVF3_RR);
		}
		op1.Free(build);
		op2.Free(build);
		ExpEmit to(build, ValueType->GetRegType(), ValueType->GetRegCount());
		build->Emit(op, to.RegNum, op1.RegNum, op2.RegNum);
		return to;
	}

	if (Operator == '*')
	{
		// Multiplication is commutative, so only the second operand may be constant.
		if (op1.Konst)
		{
			swapvalues(op1, op2);
		}
		assert(!op1.Konst);
		op1.Free(build);
		op2.Free(build);
		ExpEmit to(build, ValueType->GetRegType());
		if (ValueType->GetRegType() == REGT_FLOAT)
		{
			assert(op1.RegType == REGT_FLOAT && op2.RegType == REGT_FLOAT);
			build->Emit(op2.Konst ? OP_MULF_RK : OP_MULF_RR, to.RegNum, op1.RegNum, op2.RegNum);
			return to;
		}
		else
		{
			assert(ValueType->GetRegType() == REGT_INT);
			assert(op1.RegType == REGT_INT && op2.RegType == REGT_INT);
			build->Emit(op2.Konst ? OP_MUL_RK : OP_MUL_RR, to.RegNum, op1.RegNum, op2.RegNum);
			return to;
		}
	}
	else
	{
		// Division is not commutative, so either side may be constant (but not both).
		assert(!op1.Konst || !op2.Konst);
		assert(Operator == '%' || Operator == '/');
		op1.Free(build);
		op2.Free(build);
		ExpEmit to(build, ValueType->GetRegType());
		if (ValueType->GetRegType() == REGT_FLOAT)
		{
			assert(op1.RegType == REGT_FLOAT && op2.RegType == REGT_FLOAT);
			build->Emit(Operator == '/' ? (op1.Konst ? OP_DIVF_KR : op2.Konst ? OP_DIVF_RK : OP_DIVF_RR)
				: (op1.Konst ? OP_MODF_KR : op2.Konst ? OP_MODF_RK : OP_MODF_RR),
				to.RegNum, op1.RegNum, op2.RegNum);
			return to;
		}
		else
		{
			assert(ValueType->GetRegType() == REGT_INT);
			assert(op1.RegType == REGT_INT && op2.RegType == REGT_INT);
			if (ValueType == TypeUInt32)
			{
				build->Emit(Operator == '/' ? (op1.Konst ? OP_DIVU_KR : op2.Konst ? OP_DIVU_RK : OP_DIVU_RR)
					: (op1.Konst ? OP_MODU_KR : op2.Konst ? OP_MODU_RK : OP_MODU_RR),
					to.RegNum, op1.RegNum, op2.RegNum);
			}
			else
			{
				build->Emit(Operator == '/' ? (op1.Konst ? OP_DIV_KR : op2.Konst ? OP_DIV_RK : OP_DIV_RR)
					: (op1.Konst ? OP_MOD_KR : op2.Konst ? OP_MOD_RK : OP_MOD_RR),
					to.RegNum, op1.RegNum, op2.RegNum);
			}
			return to;
		}
	}
}

//==========================================================================
//
//
//
//==========================================================================

FxPow::FxPow(FxExpression *l, FxExpression *r)
	: FxBinary(TK_MulMul, new FxFloatCast(l), new FxFloatCast(r))
{
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxPow::Resolve(FCompileContext& ctx)
{
	CHECKRESOLVED();

	RESOLVE(left, ctx);
	RESOLVE(right, ctx);
	if (!left || !right)
	{
		delete this;
		return nullptr;
	}
	if (!left->IsNumeric() || !right->IsNumeric())
	{
		ScriptPosition.Message(MSG_ERROR, "Numeric type expected for '**'");
		delete this;
		return nullptr;
	}
	if (!left->IsFloat())
	{
		left = (new FxFloatCast(left))->Resolve(ctx);
		ABORT(left);
	}
	if (!right->IsFloat())
	{
		right = (new FxFloatCast(right))->Resolve(ctx);
		ABORT(right);
	}
	if (left->isConstant() && right->isConstant())
	{
		double v1 = static_cast<FxConstant *>(left)->GetValue().GetFloat();
		double v2 = static_cast<FxConstant *>(right)->GetValue().GetFloat();
		return new FxConstant(g_pow(v1, v2), left->ScriptPosition);
	}
	return this;
}

//==========================================================================
//
//
//
//==========================================================================

ExpEmit FxPow::Emit(VMFunctionBuilder *build)
{
	ExpEmit op1 = left->Emit(build);
	ExpEmit op2 = right->Emit(build);

	// Pow is not commutative, so either side may be constant (but not both).
	assert(!op1.Konst || !op2.Konst);
	op1.Free(build);
	op2.Free(build);
	assert(op1.RegType == REGT_FLOAT && op2.RegType == REGT_FLOAT);
	ExpEmit to(build, REGT_FLOAT);
	build->Emit((op1.Konst ? OP_POWF_KR : op2.Konst ? OP_POWF_RK : OP_POWF_RR),	to.RegNum, op1.RegNum, op2.RegNum);
	return to;
}

//==========================================================================
//
//
//
//==========================================================================

FxCompareRel::FxCompareRel(int o, FxExpression *l, FxExpression *r)
: FxBinary(o, l, r)
{
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxCompareRel::Resolve(FCompileContext& ctx)
{
	CHECKRESOLVED();

	RESOLVE(left, ctx);
	RESOLVE(right, ctx);
	if (!left || !right)
	{
		delete this;
		return nullptr;
	}

	if (left->ValueType == TypeString || right->ValueType == TypeString)
	{
		if (left->ValueType != TypeString)
		{
			left = new FxStringCast(left);
			left = left->Resolve(ctx);
			if (left == nullptr)
			{
				delete this;
				return nullptr;
			}
		}
		if (right->ValueType != TypeString)
		{
			right = new FxStringCast(right);
			right = right->Resolve(ctx);
			if (right == nullptr)
			{
				delete this;
				return nullptr;
			}
		}
		ValueType = TypeString;
	}
	else if (left->IsNumeric() && right->IsNumeric())
	{
		Promote(ctx);
	}
	else
	{
		ScriptPosition.Message(MSG_ERROR, "Incompatible operands for relative comparison");
		delete this;
		return nullptr;
	}

	if (left->isConstant() && right->isConstant())
	{
		int v;

		if (ValueType == TypeString)
		{
			FString v1 = static_cast<FxConstant *>(left)->GetValue().GetString();
			FString v2 = static_cast<FxConstant *>(right)->GetValue().GetString();
			int res = v1.Compare(v2);
			v = Operator == '<' ? res < 0 :
				Operator == '>' ? res > 0 :
				Operator == TK_Geq ? res >= 0 :
				Operator == TK_Leq ? res <= 0 : 0;
		}
		else if (IsFloat())
		{
			double v1 = static_cast<FxConstant *>(left)->GetValue().GetFloat();
			double v2 = static_cast<FxConstant *>(right)->GetValue().GetFloat();
			v =	Operator == '<'? v1 < v2 : 
				Operator == '>'? v1 > v2 : 
				Operator == TK_Geq? v1 >= v2 : 
				Operator == TK_Leq? v1 <= v2 : 0;
		}
		else if (ValueType == TypeUInt32)
		{
			int v1 = static_cast<FxConstant *>(left)->GetValue().GetUInt();
			int v2 = static_cast<FxConstant *>(right)->GetValue().GetUInt();
			v =	Operator == '<'? v1 < v2 : 
				Operator == '>'? v1 > v2 : 
				Operator == TK_Geq? v1 >= v2 : 
				Operator == TK_Leq? v1 <= v2 : 0;
		}
		else 
		{
			int v1 = static_cast<FxConstant *>(left)->GetValue().GetInt();
			int v2 = static_cast<FxConstant *>(right)->GetValue().GetInt();
			v = Operator == '<' ? v1 < v2 :
				Operator == '>' ? v1 > v2 :
				Operator == TK_Geq ? v1 >= v2 :
				Operator == TK_Leq ? v1 <= v2 : 0;
		}
		FxExpression *e = new FxConstant(v, ScriptPosition);
		delete this;
		return e;
	}
	CompareType = ValueType;	// needs to be preserved for detection of unsigned compare.
	ValueType = TypeBool;
	return this;
}


//==========================================================================
//
//
//
//==========================================================================

ExpEmit FxCompareRel::Emit(VMFunctionBuilder *build)
{
	ExpEmit op1 = left->Emit(build);
	ExpEmit op2 = right->Emit(build);
	assert(op1.RegType == op2.RegType);
	assert(!op1.Konst || !op2.Konst);

	if (op1.RegType == REGT_STRING)
	{
		ExpEmit to(build, REGT_INT);
		int a = Operator == '<' ? CMP_LT :
			Operator == '>' ? CMP_LE | CMP_CHECK :
			Operator == TK_Geq ? CMP_LT | CMP_CHECK : CMP_LE;

		if (op1.Konst)
		{
			a |= CMP_BK;
		}
		else
		{
			op1.Free(build);
		}
		if (op2.Konst)
		{
			a |= CMP_CK;
		}
		else
		{
			op2.Free(build);
		}

		build->Emit(OP_LI, to.RegNum, 0, 0);
		build->Emit(OP_CMPS, a, op1.RegNum, op2.RegNum);
		build->Emit(OP_JMP, 1);
		build->Emit(OP_LI, to.RegNum, 1);
		return to;
	}
	else
	{
		assert(op1.RegType == REGT_INT || op1.RegType == REGT_FLOAT);
		assert(Operator == '<' || Operator == '>' || Operator == TK_Geq || Operator == TK_Leq);
		static const VM_UBYTE InstrMap[][4] =
		{
			{ OP_LT_RR, OP_LTF_RR, OP_LTU_RR, 0 },	// <
			{ OP_LE_RR, OP_LEF_RR, OP_LEU_RR, 1 },	// >
			{ OP_LT_RR, OP_LTF_RR, OP_LTU_RR, 1 },	// >=
			{ OP_LE_RR, OP_LEF_RR, OP_LEU_RR, 0 }	// <=
		};
		int instr, check;
		ExpEmit to(build, REGT_INT);
		int index = Operator == '<' ? 0 :
			Operator == '>' ? 1 :
			Operator == TK_Geq ? 2 : 3;

		int mode = op1.RegType == REGT_FLOAT ? 1 : CompareType == TypeUInt32 ? 2 : 0;
		instr = InstrMap[index][mode];
		check = InstrMap[index][3];
		if (op2.Konst)
		{
			instr += 1;
		}
		else
		{
			op2.Free(build);
		}
		if (op1.Konst)
		{
			instr += 2;
		}
		else
		{
			op1.Free(build);
		}

		// See FxBoolCast for comments, since it's the same thing.
		build->Emit(OP_LI, to.RegNum, 0, 0);
		build->Emit(instr, check, op1.RegNum, op2.RegNum);
		build->Emit(OP_JMP, 1);
		build->Emit(OP_LI, to.RegNum, 1);
		return to;
	}
}

//==========================================================================
//
//
//
//==========================================================================

FxCompareEq::FxCompareEq(int o, FxExpression *l, FxExpression *r)
: FxBinary(o, l, r)
{
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxCompareEq::Resolve(FCompileContext& ctx)
{
	CHECKRESOLVED();

	RESOLVE(left, ctx);
	RESOLVE(right, ctx);
	if (!left || !right)
	{
		delete this;
		return nullptr;
	}

	if (left->ValueType != right->ValueType)	// identical types are always comparable, if they can be placed in a register, so we can save most checks if this is the case.
	{
		// Special cases: Compare strings and names with names, sounds, colors, state labels and class types.
		// These are all types a string can be implicitly cast into, so for convenience, so they should when doing a comparison.
		if ((left->ValueType == TypeString || left->ValueType == TypeName) &&
			(right->ValueType == TypeName || right->ValueType == TypeSound || right->ValueType == TypeColor || right->ValueType->IsKindOf(RUNTIME_CLASS(PClassPointer)) || right->ValueType == TypeStateLabel))
		{
			left = new FxTypeCast(left, right->ValueType, false, true);
			left = left->Resolve(ctx);
			ABORT(left);
			ValueType = right->ValueType;
		}
		else if ((right->ValueType == TypeString || right->ValueType == TypeName) &&
			(left->ValueType == TypeName || left->ValueType == TypeSound || left->ValueType == TypeColor || left->ValueType->IsKindOf(RUNTIME_CLASS(PClassPointer)) || left->ValueType == TypeStateLabel))
		{
			right = new FxTypeCast(right, left->ValueType, false, true);
			right = right->Resolve(ctx);
			ABORT(right);
			ValueType = left->ValueType;
		}
		else if (left->IsNumeric() && right->IsNumeric())
		{
			Promote(ctx);
		}
		else if (left->ValueType->GetRegType() == REGT_POINTER && right->ValueType->GetRegType() == REGT_POINTER)
		{
			if (left->ValueType != right->ValueType && right->ValueType != TypeNullPtr && left->ValueType != TypeNullPtr &&
				!AreCompatiblePointerTypes(left->ValueType, right->ValueType, true))
			{
				goto error;
			}
		}
		else
		{
			goto error;
		}
	}
	else if (left->ValueType->GetRegType() == REGT_NIL)
	{
		goto error;
	}
	else
	{
		ValueType = left->ValueType;
	}

	if (Operator == TK_ApproxEq && ValueType->GetRegType() != REGT_FLOAT && ValueType->GetRegType() != REGT_STRING)
	{
		// Only floats, vectors and strings have handling for '~==', for all other types this is an error.
		goto error;
	}

	if (left->isConstant() && right->isConstant())
	{
		int v;

		if (ValueType == TypeString)
		{
			FString v1 = static_cast<FxConstant *>(left)->GetValue().GetString();
			FString v2 = static_cast<FxConstant *>(right)->GetValue().GetString();
			if (Operator == TK_ApproxEq) v = !v1.CompareNoCase(v2);
			else
			{
				v = !!v1.Compare(v2);
				if (Operator == TK_Eq) v = !v;
			}
		}
		else if (ValueType->GetRegType() == REGT_FLOAT)
		{
			double v1 = static_cast<FxConstant *>(left)->GetValue().GetFloat();
			double v2 = static_cast<FxConstant *>(right)->GetValue().GetFloat();
			v = Operator == TK_Eq? v1 == v2 : Operator == TK_Neq? v1 != v2 : fabs(v1-v2) < VM_EPSILON;
		}
		else
		{
			int v1 = static_cast<FxConstant *>(left)->GetValue().GetInt();
			int v2 = static_cast<FxConstant *>(right)->GetValue().GetInt();
			v = Operator == TK_Eq? v1 == v2 : v1 != v2;
		}
		FxExpression *e = new FxConstant(v, ScriptPosition);
		delete this;
		return e;
	}
	else
	{
		// also simplify comparisons against zero. For these a bool cast/unary not on the other value will do just as well and create better code.
		if (Operator != TK_ApproxEq)
		{
			if (left->isConstant())
			{
				bool leftisnull;
				switch (left->ValueType->GetRegType())
				{
				case REGT_INT:
					leftisnull = static_cast<FxConstant *>(left)->GetValue().GetInt() == 0;
					break;

				case REGT_FLOAT:
					assert(left->ValueType->GetRegCount() == 1);	// vectors should not be able to get here.
					leftisnull = static_cast<FxConstant *>(left)->GetValue().GetFloat() == 0;
					break;

				case REGT_POINTER:
					leftisnull = static_cast<FxConstant *>(left)->GetValue().GetPointer() == nullptr;
					break;

				default:
					leftisnull = false;
				}
				if (leftisnull)
				{
					FxExpression *x;
					if (Operator == TK_Eq) x = new FxUnaryNotBoolean(right);
					else x = new FxBoolCast(right);
					right = nullptr;
					delete this;
					return x->Resolve(ctx);
				}

			}
			if (right->isConstant())
			{
				bool rightisnull;
				switch (right->ValueType->GetRegType())
				{
				case REGT_INT:
					rightisnull = static_cast<FxConstant *>(right)->GetValue().GetInt() == 0;
					break;

				case REGT_FLOAT:
					assert(right->ValueType->GetRegCount() == 1);	// vectors should not be able to get here.
					rightisnull = static_cast<FxConstant *>(right)->GetValue().GetFloat() == 0;
					break;

				case REGT_POINTER:
					rightisnull = static_cast<FxConstant *>(right)->GetValue().GetPointer() == nullptr;
					break;

				default:
					rightisnull = false;
				}
				if (rightisnull)
				{
					FxExpression *x;
					if (Operator == TK_Eq) x = new FxUnaryNotBoolean(left);
					else x = new FxBoolCast(left);
					left = nullptr;
					delete this;
					return x->Resolve(ctx);
				}
			}
		}
	}
	ValueType = TypeBool;
	return this;

error:
	ScriptPosition.Message(MSG_ERROR, "Incompatible operands for %s comparison", Operator == TK_Eq ? "==" : Operator == TK_Neq ? "!=" : "~==");
	delete this;
	return nullptr;
}

//==========================================================================
//
//
//
//==========================================================================

ExpEmit FxCompareEq::Emit(VMFunctionBuilder *build)
{
	ExpEmit op1 = left->Emit(build);
	ExpEmit op2 = right->Emit(build);
	assert(op1.RegType == op2.RegType);
	int instr;

	if (op1.RegType == REGT_STRING)
	{
		ExpEmit to(build, REGT_INT);
		assert(Operator == TK_Eq || Operator == TK_Neq || Operator == TK_ApproxEq);
		int a = Operator == TK_Eq ? CMP_EQ :
			Operator == TK_Neq ? CMP_EQ | CMP_CHECK : CMP_EQ | CMP_APPROX;

		if (op1.Konst) a|= CMP_BK;
		if (op2.Konst) a |= CMP_CK;

		build->Emit(OP_LI, to.RegNum, 0, 0);
		build->Emit(OP_CMPS, a, op1.RegNum, op2.RegNum);
		build->Emit(OP_JMP, 1);
		build->Emit(OP_LI, to.RegNum, 1);
		op1.Free(build);
		op2.Free(build);
		return to;
	}
	else
	{

		// Only the second operand may be constant.
		if (op1.Konst)
		{
			swapvalues(op1, op2);
		}
		assert(!op1.Konst);
		assert(op1.RegCount >= 1 && op1.RegCount <= 3);

		ExpEmit to(build, REGT_INT);

		static int flops[] = { OP_EQF_R, OP_EQV2_R, OP_EQV3_R };
		instr = op1.RegType == REGT_INT ? OP_EQ_R :
			op1.RegType == REGT_FLOAT ? flops[op1.RegCount - 1] :
			OP_EQA_R;
		op1.Free(build);
		if (!op2.Konst)
		{
			op2.Free(build);
		}
		else
		{
			instr += 1;
		}

		// See FxUnaryNotBoolean for comments, since it's the same thing.
		build->Emit(OP_LI, to.RegNum, 0, 0);
		build->Emit(instr, Operator == TK_ApproxEq ? CMP_APPROX : ((Operator != TK_Eq) ? CMP_CHECK : 0), op1.RegNum, op2.RegNum);
		build->Emit(OP_JMP, 1);
		build->Emit(OP_LI, to.RegNum, 1);
		return to;
	}
}

//==========================================================================
//
//
//
//==========================================================================

FxBitOp::FxBitOp(int o, FxExpression *l, FxExpression *r)
: FxBinary(o, l, r)
{
	ValueType = TypeSInt32;
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxBitOp::Resolve(FCompileContext& ctx)
{
	CHECKRESOLVED();

	RESOLVE(left, ctx);
	RESOLVE(right, ctx);
	if (!left || !right)
	{
		delete this;
		return nullptr;
	}

	if (left->ValueType == TypeBool && right->ValueType == TypeBool)
	{
		ValueType = TypeBool;
	}
	else if (left->IsNumeric() && right->IsNumeric())
	{
		if (!Promote(ctx, true)) return nullptr;
	}
	else
	{
		ScriptPosition.Message(MSG_ERROR, "Incompatible operands for bit operation");
		delete this;
		return nullptr;
	}

	if (left->isConstant() && right->isConstant())
	{
		int v1 = static_cast<FxConstant *>(left)->GetValue().GetInt();
		int v2 = static_cast<FxConstant *>(right)->GetValue().GetInt();

		FxExpression *e = new FxConstant(
			Operator == '&'? v1 & v2 : 
			Operator == '|'? v1 | v2 : 
			Operator == '^'? v1 ^ v2 : 0, ScriptPosition);

		delete this;
		return e;
	}
	return this;
}

//==========================================================================
//
//
//
//==========================================================================

ExpEmit FxBitOp::Emit(VMFunctionBuilder *build)
{
	assert(left->ValueType->GetRegType() == REGT_INT);
	assert(right->ValueType->GetRegType() == REGT_INT);
	int instr, rop;
	ExpEmit op1, op2;

	op1 = left->Emit(build);
	op2 = right->Emit(build);
	if (op1.Konst)
	{
		swapvalues(op1, op2);
	}
	assert(!op1.Konst);
	rop = op2.RegNum;
	op2.Free(build);
	op1.Free(build);

	instr = Operator == '&' ? OP_AND_RR :
			Operator == '|' ? OP_OR_RR :
			Operator == '^' ? OP_XOR_RR : -1;

	assert(instr > 0);
	ExpEmit to(build, REGT_INT);
	build->Emit(instr + op2.Konst, to.RegNum, op1.RegNum, rop);
	return to;
}

//==========================================================================
//
//
//
//==========================================================================

FxShift::FxShift(int o, FxExpression *l, FxExpression *r)
	: FxBinary(o, l, r)
{
	ValueType = TypeSInt32;
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxShift::Resolve(FCompileContext& ctx)
{
	CHECKRESOLVED();
	RESOLVE(left, ctx);
	RESOLVE(right, ctx);
	if (!left || !right)
	{
		delete this;
		return nullptr;
	}

	if (left->IsNumeric() && right->IsNumeric())
	{
		if (!Promote(ctx, true)) return nullptr;
		if (ValueType == TypeUInt32 && Operator == TK_RShift) Operator = TK_URShift;
	}
	else
	{
		ScriptPosition.Message(MSG_ERROR, "Incompatible operands for shift operation");
		delete this;
		return nullptr;
	}

	if (left->isConstant() && right->isConstant())
	{
		int v1 = static_cast<FxConstant *>(left)->GetValue().GetInt();
		int v2 = static_cast<FxConstant *>(right)->GetValue().GetInt();

		FxExpression *e = new FxConstant(
			Operator == TK_LShift ? v1 << v2 :
			Operator == TK_RShift ? v1 >> v2 :
			Operator == TK_URShift ? int((unsigned int)(v1) >> v2) : 0, ScriptPosition);

		delete this;
		return e;
	}
	return this;
}

//==========================================================================
//
//
//
//==========================================================================

ExpEmit FxShift::Emit(VMFunctionBuilder *build)
{
	assert(left->ValueType->GetRegType() == REGT_INT);
	assert(right->ValueType->GetRegType() == REGT_INT);
	static const VM_UBYTE InstrMap[][4] =
	{
		{ OP_SLL_RR, OP_SLL_KR, OP_SLL_RI },	// TK_LShift
		{ OP_SRA_RR, OP_SRA_KR, OP_SRA_RI },	// TK_RShift
		{ OP_SRL_RR, OP_SRL_KR, OP_SRL_RI },	// TK_URShift
	};
	int index, instr, rop;
	ExpEmit op1, op2;

	index = Operator == TK_LShift ? 0 :
			Operator == TK_RShift ? 1 :
			Operator == TK_URShift ? 2 : -1;
	assert(index >= 0);
	op1 = left->Emit(build);

	// Shift instructions use right-hand immediates instead of constant registers.
	if (right->isConstant())
	{
		rop = static_cast<FxConstant *>(right)->GetValue().GetInt();
		op2.Konst = true;
	}
	else
	{
		op2 = right->Emit(build);
		assert(!op2.Konst);
		op2.Free(build);
		rop = op2.RegNum;
	}

	if (!op1.Konst)
	{
		op1.Free(build);
		instr = InstrMap[index][op2.Konst? 2:0];
	}
	else
	{
		assert(!op2.Konst);
		instr = InstrMap[index][1];
	}
	assert(instr != 0);
	ExpEmit to(build, REGT_INT);
	build->Emit(instr, to.RegNum, op1.RegNum, rop);
	return to;
}

//==========================================================================
//
//
//
//==========================================================================

FxLtGtEq::FxLtGtEq(FxExpression *l, FxExpression *r)
	: FxBinary(TK_LtGtEq, l, r)
{
	ValueType = TypeSInt32;
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxLtGtEq::Resolve(FCompileContext& ctx)
{
	CHECKRESOLVED();

	RESOLVE(left, ctx);
	RESOLVE(right, ctx);
	if (!left || !right)
	{
		delete this;
		return nullptr;
	}

	if (left->IsNumeric() && right->IsNumeric())
	{
		Promote(ctx);
	}
	else
	{
		ScriptPosition.Message(MSG_ERROR, "<>= expects two numeric operands");
		delete this;
		return nullptr;
	}

	if (left->isConstant() && right->isConstant())
	{
		// let's cut this short and always compare doubles. For integers the result will be exactly the same as with an integer comparison, either signed or unsigned.
		auto v1 = static_cast<FxConstant *>(left)->GetValue().GetFloat();
		auto v2 = static_cast<FxConstant *>(right)->GetValue().GetFloat();
		auto e = new FxConstant(v1 < v2 ? -1 : v1 > v2 ? 1 : 0, ScriptPosition);
		delete this;
		return e;
	}
	return this;
}

//==========================================================================
//
//
//
//==========================================================================

ExpEmit FxLtGtEq::Emit(VMFunctionBuilder *build)
{
	ExpEmit op1 = left->Emit(build);
	ExpEmit op2 = right->Emit(build);

	assert(op1.RegType == op2.RegType);
	assert(op1.RegType == REGT_INT || op1.RegType == REGT_FLOAT);
	assert(!op1.Konst || !op2.Konst);

	ExpEmit to(build, REGT_INT);

	int instr = op1.RegType == REGT_INT ? (left->ValueType == TypeUInt32? OP_LTU_RR : OP_LT_RR) : OP_LTF_RR;
	if (op1.Konst) instr += 2;
	if (op2.Konst) instr++;


	build->Emit(OP_LI, to.RegNum, 1);										// default to 1
	build->Emit(instr, 0, op1.RegNum, op2.RegNum);							// if (left < right)
	auto j1 = build->Emit(OP_JMP, 1);
	build->Emit(OP_LI, to.RegNum, -1);										// result is -1
	auto j2 = build->Emit(OP_JMP, 1);										// jump to end
	build->BackpatchToHere(j1);
	build->Emit(instr + OP_LE_RR - OP_LT_RR, 0, op1.RegNum, op2.RegNum);	// if (left == right)
	auto j3 = build->Emit(OP_JMP, 1);
	build->Emit(OP_LI, to.RegNum, 0);										// result is 0
	build->BackpatchToHere(j2);
	build->BackpatchToHere(j3);

	return to;
}

//==========================================================================
//
//
//
//==========================================================================

FxConcat::FxConcat(FxExpression *l, FxExpression *r)
	: FxBinary(TK_DotDot, l, r)
{
	ValueType = TypeString;
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxConcat::Resolve(FCompileContext& ctx)
{
	CHECKRESOLVED();

	RESOLVE(left, ctx);
	RESOLVE(right, ctx);
	if (!left || !right)
	{
		delete this;
		return nullptr;
	}

	// To concatenate two operands the only requirement is that they are integral types, i.e. can occupy a register
	if (left->ValueType->GetRegType() == REGT_NIL || right->ValueType->GetRegType() == REGT_NIL)
	{
		ScriptPosition.Message(MSG_ERROR, "Invalid operand for string concatenation");
		delete this;
		return nullptr;
	}

	if (left->isConstant() && right->isConstant() && (left->ValueType == TypeString || left->ValueType == TypeName) && (right->ValueType == TypeString || right->ValueType == TypeName))
	{
		// for now this is only types which have a constant string representation.
		auto v1 = static_cast<FxConstant *>(left)->GetValue().GetString();
		auto v2 = static_cast<FxConstant *>(right)->GetValue().GetString();
		auto e = new FxConstant(v1 + v2, ScriptPosition);
		delete this;
		return e;
	}
	return this;
}

//==========================================================================
//
//
//
//==========================================================================

ExpEmit FxConcat::Emit(VMFunctionBuilder *build)
{
	ExpEmit op1 = left->Emit(build);
	ExpEmit op2 = right->Emit(build);
	ExpEmit strng, strng2;

	if (op1.RegType == REGT_STRING && op1.Konst)
	{
		strng = ExpEmit(build, REGT_STRING);
		build->Emit(OP_LKS, strng.RegNum, op1.RegNum);
	}
	else if (op1.RegType == REGT_STRING)
	{
		strng = op1;
	}
	else
	{
		int cast;
		strng = ExpEmit(build, REGT_STRING);
		if (op1.Konst)
		{
			ExpEmit nonconst(build, op1.RegType);
			build->Emit(op1.RegType == REGT_INT ? OP_LK : op1.RegType == REGT_FLOAT ? OP_LKF : OP_LKP, nonconst.RegNum, op1.RegNum);
			op1 = nonconst;
		}
		if (op1.RegType == REGT_FLOAT) cast = op1.RegCount == 1 ? CAST_F2S : op1.RegCount == 2 ? CAST_V22S : CAST_V32S;
		else if (left->ValueType == TypeUInt32) cast = CAST_U2S;
		else if (left->ValueType == TypeName) cast = CAST_N2S;
		else if (left->ValueType == TypeSound) cast = CAST_So2S;
		else if (left->ValueType == TypeColor) cast = CAST_Co2S;
		else if (left->ValueType == TypeSpriteID) cast = CAST_SID2S;
		else if (left->ValueType == TypeTextureID) cast = CAST_TID2S;
		else if (op1.RegType == REGT_POINTER) cast = CAST_P2S;
		else if (op1.RegType == REGT_INT) cast = CAST_I2S;
		else assert(false && "Bad type for string concatenation");
		build->Emit(OP_CAST, strng.RegNum, op1.RegNum, cast);
		op1.Free(build);
	}

	if (op2.RegType == REGT_STRING && op2.Konst)
	{
		strng2 = ExpEmit(build, REGT_STRING);
		build->Emit(OP_LKS, strng2.RegNum, op2.RegNum);
	}
	else if (op2.RegType == REGT_STRING)
	{
		strng2 = op2;
	}
	else
	{
		int cast;
		strng2 = ExpEmit(build, REGT_STRING);
		if (op2.Konst)
		{
			ExpEmit nonconst(build, op2.RegType);
			build->Emit(op2.RegType == REGT_INT ? OP_LK : op2.RegType == REGT_FLOAT ? OP_LKF : OP_LKP, nonconst.RegNum, op2.RegNum);
			op2 = nonconst;
		}
		if (op2.RegType == REGT_FLOAT) cast = op2.RegCount == 1 ? CAST_F2S : op2.RegCount == 2 ? CAST_V22S : CAST_V32S;
		else if (right->ValueType == TypeUInt32) cast = CAST_U2S;
		else if (right->ValueType == TypeName) cast = CAST_N2S;
		else if (right->ValueType == TypeSound) cast = CAST_So2S;
		else if (right->ValueType == TypeColor) cast = CAST_Co2S;
		else if (right->ValueType == TypeSpriteID) cast = CAST_SID2S;
		else if (right->ValueType == TypeTextureID) cast = CAST_TID2S;
		else if (op2.RegType == REGT_POINTER) cast = CAST_P2S;
		else if (op2.RegType == REGT_INT) cast = CAST_I2S;
		else assert(false && "Bad type for string concatenation");
		build->Emit(OP_CAST, strng2.RegNum, op2.RegNum, cast);
		op2.Free(build);
	}
	strng.Free(build);
	strng2.Free(build);
	ExpEmit dest(build, REGT_STRING);
	assert(strng.RegType == strng2.RegType && strng.RegType == REGT_STRING);
	build->Emit(OP_CONCAT, dest.RegNum, strng.RegNum, strng2.RegNum);
	return dest;
}

//==========================================================================
//
//
//
//==========================================================================

FxBinaryLogical::FxBinaryLogical(int o, FxExpression *l, FxExpression *r)
: FxExpression(EFX_BinaryLogical, l->ScriptPosition)
{
	Operator=o;
	left=l;
	right=r;
	ValueType = TypeBool;
}

//==========================================================================
//
//
//
//==========================================================================

FxBinaryLogical::~FxBinaryLogical()
{
	SAFE_DELETE(left);
	SAFE_DELETE(right);
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxBinaryLogical::Resolve(FCompileContext& ctx)
{
	CHECKRESOLVED();
	RESOLVE(left, ctx);
	RESOLVE(right, ctx);
	ABORT(right && left);

	if (left->ValueType != TypeBool)
	{
		left = new FxBoolCast(left);
		SAFE_RESOLVE(left, ctx);
	}
	if (right->ValueType != TypeBool)
	{
		right = new FxBoolCast(right);
		SAFE_RESOLVE(right, ctx);
	}

	int b_left=-1, b_right=-1;
	if (left->isConstant()) b_left = static_cast<FxConstant *>(left)->GetValue().GetBool();
	if (right->isConstant()) b_right = static_cast<FxConstant *>(right)->GetValue().GetBool();

	// Do some optimizations. This will throw out all sub-expressions that are not
	// needed to retrieve the final result.
	if (Operator == TK_AndAnd)
	{
		if (b_left==0 || b_right==0)
		{
			FxExpression *x = new FxConstant(true, ScriptPosition);
			delete this;
			return x;
		}
		else if (b_left==1 && b_right==1)
		{
			FxExpression *x = new FxConstant(false, ScriptPosition);
			delete this;
			return x;
		}
		else if (b_left==1)
		{
			FxExpression *x = right;
			right=nullptr;
			delete this;
			return x;
		}
		else if (b_right==1)
		{
			FxExpression *x = left;
			left=nullptr;
			delete this;
			return x;
		}
	}
	else if (Operator == TK_OrOr)
	{
		if (b_left==1 || b_right==1)
		{
			FxExpression *x = new FxConstant(true, ScriptPosition);
			delete this;
			return x;
		}
		if (b_left==0 && b_right==0)
		{
			FxExpression *x = new FxConstant(false, ScriptPosition);
			delete this;
			return x;
		}
		else if (b_left==0)
		{
			FxExpression *x = right;
			right=nullptr;
			delete this;
			return x;
		}
		else if (b_right==0)
		{
			FxExpression *x = left;
			left=nullptr;
			delete this;
			return x;
		}
	}
	Flatten();
	return this;
}

//==========================================================================
//
// flatten a list of the same operator into a single node.
//
//==========================================================================

void FxBinaryLogical::Flatten()
{
	if (left->ExprType == EFX_BinaryLogical && static_cast<FxBinaryLogical *>(left)->Operator == Operator)
	{
		list = std::move(static_cast<FxBinaryLogical *>(left)->list);
		delete left;
	}
	else
	{
		list.Push(left);
	}

	if (right->ExprType == EFX_BinaryLogical && static_cast<FxBinaryLogical *>(right)->Operator == Operator)
	{
		auto &rlist = static_cast<FxBinaryLogical *>(right)->list;
		auto cnt = rlist.Size();
		auto v = list.Reserve(cnt);
		for (unsigned i = 0; i < cnt; i++)
		{
			list[v + i] = rlist[i];
			rlist[i] = nullptr;
		}
		delete right;
	}
	else
	{
		list.Push(right);
	}
	left = right = nullptr;
}

//==========================================================================
//
//
//
//==========================================================================

ExpEmit FxBinaryLogical::Emit(VMFunctionBuilder *build)
{
	TArray<size_t> patchspots;

	int zero = build->GetConstantInt(0);
	for (unsigned i = 0; i < list.Size(); i++)
	{
		assert(list[i]->ValueType->GetRegType() == REGT_INT);
		ExpEmit op1 = list[i]->Emit(build);
		assert(!op1.Konst);
		op1.Free(build);
		build->Emit(OP_EQ_K, (Operator == TK_AndAnd) ? 1 : 0, op1.RegNum, zero);
		patchspots.Push(build->Emit(OP_JMP, 0, 0, 0));
	}
	ExpEmit to(build, REGT_INT);
	build->Emit(OP_LI, to.RegNum, (Operator == TK_AndAnd) ? 1 : 0);
	build->Emit(OP_JMP, 1);
	auto ctarget = build->Emit(OP_LI, to.RegNum, (Operator == TK_AndAnd) ? 0 : 1);
	for (auto addr : patchspots) build->Backpatch(addr, ctarget);
	list.DeleteAndClear();
	list.ShrinkToFit();
	return to;
}

//==========================================================================
//
//
//
//==========================================================================

FxDotCross::FxDotCross(int o, FxExpression *l, FxExpression *r)
	: FxExpression(EFX_DotCross, l->ScriptPosition)
{
	Operator = o;
	left = l;
	right = r;
}

//==========================================================================
//
//
//
//==========================================================================

FxDotCross::~FxDotCross()
{
	SAFE_DELETE(left);
	SAFE_DELETE(right);
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxDotCross::Resolve(FCompileContext& ctx)
{
	CHECKRESOLVED();
	RESOLVE(left, ctx);
	RESOLVE(right, ctx);
	ABORT(right && left);

	if (!left->IsVector() || left->ValueType != right->ValueType || (Operator == TK_Cross && left->ValueType != TypeVector3))
	{
		ScriptPosition.Message(MSG_ERROR, "Incompatible operants for %sproduct", Operator == TK_Cross ? "cross-" : "dot-");
		delete this;
		return nullptr;
	}
	ValueType = Operator == TK_Cross ? (PType*)TypeVector3 : TypeFloat64;
	return this;
}

//==========================================================================
//
//
//
//==========================================================================

ExpEmit FxDotCross::Emit(VMFunctionBuilder *build)
{
	ExpEmit to(build, ValueType->GetRegType(), ValueType->GetRegCount());
	ExpEmit op1 = left->Emit(build);
	ExpEmit op2 = right->Emit(build);
	int op = Operator == TK_Cross ? OP_CROSSV_RR : left->ValueType == TypeVector3 ? OP_DOTV3_RR : OP_DOTV2_RR;
	build->Emit(op, to.RegNum, op1.RegNum, op2.RegNum);
	op1.Free(build);
	op2.Free(build);
	return to;
}

//==========================================================================
//
//
//
//==========================================================================

FxTypeCheck::FxTypeCheck(FxExpression *l, FxExpression *r)
	: FxExpression(EFX_TypeCheck, l->ScriptPosition)
{
	left = new FxTypeCast(l, NewPointer(RUNTIME_CLASS(DObject)), false);
	right = new FxClassTypeCast(NewClassPointer(RUNTIME_CLASS(DObject)), r);
	EmitTail = false;
	ValueType = TypeBool;
}

//==========================================================================
//
//
//
//==========================================================================

FxTypeCheck::~FxTypeCheck()
{
	SAFE_DELETE(left);
	SAFE_DELETE(right);
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxTypeCheck::Resolve(FCompileContext& ctx)
{
	CHECKRESOLVED();
	RESOLVE(left, ctx);
	RESOLVE(right, ctx);
	ABORT(right && left);
	return this;
}

//==========================================================================
//
//
//
//==========================================================================

PPrototype *FxTypeCheck::ReturnProto()
{
	EmitTail = true;
	return FxExpression::ReturnProto();
}


//==========================================================================
//
//
//
//==========================================================================

int BuiltinTypeCheck(VMValue *param, TArray<VMValue> &defaultparam, int numparam, VMReturn *ret, int numret)
{
	assert(numparam == 2);
	PARAM_POINTER_AT(0, obj, DObject);
	PARAM_CLASS_AT(1, cls, DObject);
	ACTION_RETURN_BOOL(obj && obj->IsKindOf(cls));
}

//==========================================================================
//
//
//
//==========================================================================

ExpEmit FxTypeCheck::Emit(VMFunctionBuilder *build)
{
	EmitParameter(build, left, ScriptPosition);
	EmitParameter(build, right, ScriptPosition);


	PSymbol *sym = FindBuiltinFunction(NAME_BuiltinTypeCheck, BuiltinTypeCheck);

	assert(sym->IsKindOf(RUNTIME_CLASS(PSymbolVMFunction)));
	assert(((PSymbolVMFunction *)sym)->Function != nullptr);
	auto callfunc = ((PSymbolVMFunction *)sym)->Function;

	int opcode = (EmitTail ? OP_TAIL_K : OP_CALL_K);
	build->Emit(opcode, build->GetConstantAddress(callfunc, ATAG_OBJECT), 2, 1);

	if (EmitTail)
	{
		ExpEmit call;
		call.Final = true;
		return call;
	}

	ExpEmit out(build, REGT_INT);
	build->Emit(OP_RESULT, 0, REGT_INT, out.RegNum);
	return out;
}

//==========================================================================
//
//
//
//==========================================================================

FxDynamicCast::FxDynamicCast(PClass * cls, FxExpression *r)
	: FxExpression(EFX_DynamicCast, r->ScriptPosition)
{
	expr = r;
	CastType = cls;
}

//==========================================================================
//
//
//
//==========================================================================

FxDynamicCast::~FxDynamicCast()
{
	SAFE_DELETE(expr);
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxDynamicCast::Resolve(FCompileContext& ctx)
{
	CHECKRESOLVED();
	SAFE_RESOLVE(expr, ctx);
	if (expr->ExprType == EFX_GetDefaultByType)
	{
		int a = 0;
	}
	bool constflag = expr->ValueType->IsKindOf(RUNTIME_CLASS(PPointer)) && static_cast<PPointer *>(expr->ValueType)->IsConst;
	if (constflag)
	{
		// readonly pointers are normally only used for class defaults which lack type information to be cast properly, so we have to error out here.
		ScriptPosition.Message(MSG_ERROR, "Cannot cast a readonly pointer");
		delete this;
		return nullptr;
	}
	expr = new FxTypeCast(expr, NewPointer(RUNTIME_CLASS(DObject), constflag), true, true);
	expr = expr->Resolve(ctx);
	if (expr == nullptr)
	{
		delete this;
		return nullptr;
	}
	ValueType = NewPointer(CastType, constflag);
	return this;
}

//==========================================================================
//
//
//
//==========================================================================

ExpEmit FxDynamicCast::Emit(VMFunctionBuilder *build)
{
	ExpEmit in = expr->Emit(build);
	ExpEmit out = in.Fixed ? ExpEmit(build, in.RegType) : in;
	ExpEmit check(build, REGT_INT);
	assert(out.RegType == REGT_POINTER);

	if (in.Fixed) build->Emit(OP_MOVEA, out.RegNum, in.RegNum);
	build->Emit(OP_PARAM, 0, REGT_POINTER, in.RegNum);
	build->Emit(OP_PARAM, 0, REGT_POINTER | REGT_KONST, build->GetConstantAddress(CastType, ATAG_OBJECT));

	PSymbol *sym = FindBuiltinFunction(NAME_BuiltinTypeCheck, BuiltinTypeCheck);
	assert(sym->IsKindOf(RUNTIME_CLASS(PSymbolVMFunction)));
	assert(((PSymbolVMFunction *)sym)->Function != nullptr);
	auto callfunc = ((PSymbolVMFunction *)sym)->Function;

	build->Emit(OP_CALL_K, build->GetConstantAddress(callfunc, ATAG_OBJECT), 2, 1);
	build->Emit(OP_RESULT, 0, REGT_INT, check.RegNum);
	build->Emit(OP_EQ_K, 0, check.RegNum, build->GetConstantInt(0));
	auto patch = build->Emit(OP_JMP, 0);
	build->Emit(OP_LKP, out.RegNum, build->GetConstantAddress(nullptr, ATAG_OBJECT));
	build->BackpatchToHere(patch);
	return out;
}

//==========================================================================
//
//
//
//==========================================================================

FxConditional::FxConditional(FxExpression *c, FxExpression *t, FxExpression *f)
: FxExpression(EFX_Conditional, c->ScriptPosition)
{
	condition = c;
	truex=t;
	falsex=f;
}

//==========================================================================
//
//
//
//==========================================================================

FxConditional::~FxConditional()
{
	SAFE_DELETE(condition);
	SAFE_DELETE(truex);
	SAFE_DELETE(falsex);
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxConditional::Resolve(FCompileContext& ctx)
{
	CHECKRESOLVED();
	RESOLVE(condition, ctx);
	RESOLVE(truex, ctx);
	RESOLVE(falsex, ctx);
	ABORT(condition && truex && falsex);

	if (truex->ValueType == falsex->ValueType)
		ValueType = truex->ValueType;
	else if (truex->ValueType == TypeBool && falsex->ValueType == TypeBool)
		ValueType = TypeBool;
	else if (truex->IsInteger() && falsex->IsInteger())
		ValueType = TypeSInt32;
	else if (truex->IsNumeric() && falsex->IsNumeric())
		ValueType = TypeFloat64;
	else if (truex->IsPointer() && falsex->ValueType == TypeNullPtr)
		ValueType = truex->ValueType;
	else if (falsex->IsPointer() && truex->ValueType == TypeNullPtr)
		ValueType = falsex->ValueType;
	else
		ValueType = TypeVoid;
	//else if (truex->ValueType != falsex->ValueType)

	if (ValueType->GetRegType() == REGT_NIL)
	{
		ScriptPosition.Message(MSG_ERROR, "Incompatible types for ?: operator");
		delete this;
		return nullptr;
	}

	if (condition->ValueType != TypeBool)
	{
		condition = new FxBoolCast(condition);
		SAFE_RESOLVE(condition, ctx);
	}

	if (condition->isConstant())
	{
		ExpVal condval = static_cast<FxConstant *>(condition)->GetValue();
		bool result = condval.GetBool();

		FxExpression *e = result? truex:falsex;
		delete (result? falsex:truex);
		falsex = truex = nullptr;
		delete this;
		return e;
	}

	if (IsFloat())
	{
		if (truex->ValueType->GetRegType() != REGT_FLOAT)
		{
			truex = new FxFloatCast(truex);
			RESOLVE(truex, ctx);
		}
		if (falsex->ValueType->GetRegType() != REGT_FLOAT)
		{
			falsex = new FxFloatCast(falsex);
			RESOLVE(falsex, ctx);
		}
	}

	return this;
}

//==========================================================================
//
//
//
//==========================================================================

ExpEmit FxConditional::Emit(VMFunctionBuilder *build)
{
	size_t truejump, falsejump;
	ExpEmit out;

	// The true and false expressions ought to be assigned to the
	// same temporary instead of being copied to it. Oh well; good enough
	// for now.
	ExpEmit cond = condition->Emit(build);
	assert(cond.RegType == REGT_INT && !cond.Konst);

	// Test condition.
	build->Emit(OP_EQ_K, 1, cond.RegNum, build->GetConstantInt(0));
	falsejump = build->Emit(OP_JMP, 0);
	cond.Free(build);

	// Evaluate true expression.
	if (truex->isConstant() && truex->ValueType->GetRegType() == REGT_INT)
	{
		out = ExpEmit(build, REGT_INT);
		build->EmitLoadInt(out.RegNum, static_cast<FxConstant *>(truex)->GetValue().GetInt());
	}
	else
	{
		ExpEmit trueop = truex->Emit(build);
		if (trueop.Konst)
		{
			trueop.Free(build);
			if (trueop.RegType == REGT_FLOAT)
			{
				out = ExpEmit(build, REGT_FLOAT);
				build->Emit(OP_LKF, out.RegNum, trueop.RegNum);
			}
			else if (trueop.RegType == REGT_POINTER)
			{
				out = ExpEmit(build, REGT_POINTER);
				build->Emit(OP_LKP, out.RegNum, trueop.RegNum);
			}
			else
			{
				assert(trueop.RegType == REGT_STRING);
				out = ExpEmit(build, REGT_STRING);
				build->Emit(OP_LKS, out.RegNum, trueop.RegNum);
			}
		}
		else
		{
			// Use the register returned by the true condition as the
			// target for the false condition.
			out = trueop;
		}
	}
	// Make sure to skip the false path.
	truejump = build->Emit(OP_JMP, 0);

	// Evaluate false expression.
	build->BackpatchToHere(falsejump);
	if (falsex->isConstant() && falsex->ValueType->GetRegType() == REGT_INT)
	{
		build->EmitLoadInt(out.RegNum, static_cast<FxConstant *>(falsex)->GetValue().GetInt());
	}
	else
	{
		ExpEmit falseop = falsex->Emit(build);
		if (falseop.Konst)
		{
			if (falseop.RegType == REGT_FLOAT)
			{
				build->Emit(OP_LKF, out.RegNum, falseop.RegNum);
			}
			else if (falseop.RegType == REGT_POINTER)
			{
				build->Emit(OP_LKP, out.RegNum, falseop.RegNum);
			}
			else
			{
				assert(falseop.RegType == REGT_STRING);
				build->Emit(OP_LKS, out.RegNum, falseop.RegNum);
			}
			falseop.Free(build);
		}
		else
		{
			// Move result from the register returned by "false" to the one
			// returned by "true" so that only one register is returned by
			// this tree.
			falseop.Free(build);
			build->Emit(falsex->ValueType->GetMoveOp(), out.RegNum, falseop.RegNum, 0);
		}
	}
	build->BackpatchToHere(truejump);

	return out;
}

//==========================================================================
//
//
//
//==========================================================================
FxAbs::FxAbs(FxExpression *v)
: FxExpression(EFX_Abs, v->ScriptPosition)
{
	val = v;
	ValueType = v->ValueType;
}

//==========================================================================
//
//
//
//==========================================================================

FxAbs::~FxAbs()
{
	SAFE_DELETE(val);
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxAbs::Resolve(FCompileContext &ctx)
{
	CHECKRESOLVED();
	SAFE_RESOLVE(val, ctx);


	if (!val->IsNumeric())
	{
		ScriptPosition.Message(MSG_ERROR, "Numeric type expected");
		delete this;
		return nullptr;
	}
	else if (val->isConstant())
	{
		ExpVal value = static_cast<FxConstant *>(val)->GetValue();
		switch (value.Type->GetRegType())
		{
		case REGT_INT:
			value.Int = abs(value.Int);
			break;

		case REGT_FLOAT:
			value.Float = fabs(value.Float);
			break;

		default:
			// shouldn't happen
			delete this;
			return nullptr;
		}
		FxExpression *x = new FxConstant(value, ScriptPosition);
		delete this;
		return x;
	}
	ValueType = val->ValueType;
	return this;
}

//==========================================================================
//
//
//
//==========================================================================

ExpEmit FxAbs::Emit(VMFunctionBuilder *build)
{
	assert(ValueType == val->ValueType);
	ExpEmit from = val->Emit(build);
	ExpEmit to;
	assert(from.Konst == 0);
	assert(ValueType->GetRegCount() == 1);
	// Do it in-place, unless a local variable
	if (from.Fixed)
	{
		to = ExpEmit(build, from.RegType);
		from.Free(build);
	}
	else
	{
		to = from;
	}

	if (ValueType->GetRegType() == REGT_INT)
	{
		build->Emit(OP_ABS, to.RegNum, from.RegNum, 0);
	}
	else
	{
		build->Emit(OP_FLOP, to.RegNum, from.RegNum, FLOP_ABS);
	}
	return to;
}

//==========================================================================
//
//
//
//==========================================================================
FxATan2::FxATan2(FxExpression *y, FxExpression *x, const FScriptPosition &pos)
: FxExpression(EFX_ATan2, pos)
{
	yval = y;
	xval = x;
}

//==========================================================================
//
//
//
//==========================================================================
FxATan2::~FxATan2()
{
	SAFE_DELETE(yval);
	SAFE_DELETE(xval);
}

//==========================================================================
//
//
//
//==========================================================================
FxExpression *FxATan2::Resolve(FCompileContext &ctx)
{
	CHECKRESOLVED();
	SAFE_RESOLVE(yval, ctx);
	SAFE_RESOLVE(xval, ctx);

	if (!yval->IsNumeric() || !xval->IsNumeric())
	{
		ScriptPosition.Message(MSG_ERROR, "numeric value expected for parameter");
		delete this;
		return nullptr;
	}
	if (yval->isConstant() && xval->isConstant())
	{
		double y = static_cast<FxConstant *>(yval)->GetValue().GetFloat();
		double x = static_cast<FxConstant *>(xval)->GetValue().GetFloat();
		FxExpression *z = new FxConstant(g_atan2(y, x) * (180 / M_PI), ScriptPosition);
		delete this;
		return z;
	}
	if (yval->ValueType->GetRegType() != REGT_FLOAT && !yval->isConstant())
	{
		yval = new FxFloatCast(yval);
	}
	if (xval->ValueType->GetRegType() != REGT_FLOAT && !xval->isConstant())
	{
		xval = new FxFloatCast(xval);
	}
	ValueType = TypeFloat64;
	return this;
}

//==========================================================================
//
//
//
//==========================================================================
ExpEmit FxATan2::Emit(VMFunctionBuilder *build)
{
	ExpEmit yreg = ToReg(build, yval);
	ExpEmit xreg = ToReg(build, xval);
	yreg.Free(build);
	xreg.Free(build);
	ExpEmit out(build, REGT_FLOAT);
	build->Emit(OP_ATAN2, out.RegNum, yreg.RegNum, xreg.RegNum);
	return out;
}

//==========================================================================
//
// The atan2 opcode only takes registers as parameters, so any constants
// must be loaded into registers first.
//
//==========================================================================
ExpEmit FxATan2::ToReg(VMFunctionBuilder *build, FxExpression *val)
{
	if (val->isConstant())
	{
		ExpEmit reg(build, REGT_FLOAT);
		build->Emit(OP_LKF, reg.RegNum, build->GetConstantFloat(static_cast<FxConstant*>(val)->GetValue().GetFloat()));
		return reg;
	}
	return val->Emit(build);
}

//==========================================================================
//
//
//
//==========================================================================
FxMinMax::FxMinMax(TArray<FxExpression*> &expr, FName type, const FScriptPosition &pos)
: FxExpression(EFX_MinMax, pos), Type(type)
{
	assert(expr.Size() > 0);
	assert(type == NAME_Min || type == NAME_Max);

	choices.Resize(expr.Size());
	for (unsigned i = 0; i < expr.Size(); ++i)
	{
		choices[i] = expr[i];
		expr[i] = nullptr;
	}
}

//==========================================================================
//
//
//
//==========================================================================
FxExpression *FxMinMax::Resolve(FCompileContext &ctx)
{
	unsigned int i;
	int intcount, floatcount;

	CHECKRESOLVED();

	// Determine if float or int
	intcount = floatcount = 0;
	for (i = 0; i < choices.Size(); ++i)
	{
		RESOLVE(choices[i], ctx);
		ABORT(choices[i]);

		if (choices[i]->IsFloat())
		{
			floatcount++;
		}
		else if (choices[i]->IsInteger())
		{
			intcount++;
		}
		else
		{
			ScriptPosition.Message(MSG_ERROR, "Arguments must be of type int or float");
			delete this;
			return nullptr;
		}
	}
	if (floatcount != 0)
	{
		ValueType = TypeFloat64;
		if (intcount != 0)
		{ // There are some ints that need to be cast to floats
			for (i = 0; i < choices.Size(); ++i)
			{
				if (choices[i]->ValueType->GetRegType() == REGT_INT)
				{
					choices[i] = new FxFloatCast(choices[i]);
					RESOLVE(choices[i], ctx);
					ABORT(choices[i]);
				}
			}
		}
	}
	else
	{
		ValueType = TypeSInt32;
	}

	// If at least two arguments are constants, they can be solved now.

	// Look for first constant
	for (i = 0; i < choices.Size(); ++i)
	{
		if (choices[i]->isConstant())
		{
			ExpVal best = static_cast<FxConstant *>(choices[i])->GetValue();
			// Compare against remaining constants, which are removed.
			// The best value gets stored in this one.
			for (unsigned j = i + 1; j < choices.Size(); )
			{
				if (!choices[j]->isConstant())
				{
					j++;
				}
				else
				{
					ExpVal value = static_cast<FxConstant *>(choices[j])->GetValue();
					assert(value.Type == ValueType);
					if (Type == NAME_Min)
					{
						if (value.Type->GetRegType() == REGT_FLOAT)
						{
							if (value.Float < best.Float)
							{
								best.Float = value.Float;
							}
						}
						else
						{
							if (value.Int < best.Int)
							{
								best.Int = value.Int;
							}
						}
					}
					else
					{
						if (value.Type->GetRegType() == REGT_FLOAT)
						{
							if (value.Float > best.Float)
							{
								best.Float = value.Float;
							}
						}
						else
						{
							if (value.Int > best.Int)
							{
								best.Int = value.Int;
							}
						}
					}
					delete choices[j];
					choices[j] = nullptr;
					choices.Delete(j);
				}
			}
			FxExpression *x = new FxConstant(best, ScriptPosition);
			if (i == 0 && choices.Size() == 1)
			{ // Every choice was constant
				delete this;
				return x;
			}
			delete choices[i];
			choices[i] = x;
			break;
		}
	}
	return this;
}

//==========================================================================
//
//
//
//==========================================================================
static void EmitLoad(VMFunctionBuilder *build, const ExpEmit resultreg, const ExpVal &value)
{
	if (resultreg.RegType == REGT_FLOAT)
	{
		build->Emit(OP_LKF, resultreg.RegNum, build->GetConstantFloat(value.GetFloat()));
	}
	else
	{
		build->EmitLoadInt(resultreg.RegNum, value.GetInt());
	}
}

ExpEmit FxMinMax::Emit(VMFunctionBuilder *build)
{
	unsigned i;
	int opcode;

	assert(choices.Size() > 0);
	assert(OP_MAXF_RK == OP_MAXF_RR+1);
	assert(OP_MAX_RK == OP_MAX_RR+1);
	assert(OP_MIN_RK == OP_MIN_RR+1);
	assert(OP_MIN_RK == OP_MIN_RR+1);

	if (Type == NAME_Min)
	{
		opcode = ValueType->GetRegType() == REGT_FLOAT ? OP_MINF_RR : OP_MIN_RR;
	}
	else
	{
		opcode = ValueType->GetRegType() == REGT_FLOAT ? OP_MAXF_RR : OP_MAX_RR;
	}

	ExpEmit bestreg;

	// Get first value into a register. This will also be the result register.
	if (choices[0]->isConstant())
	{
		bestreg = ExpEmit(build, ValueType->GetRegType());
		EmitLoad(build, bestreg, static_cast<FxConstant *>(choices[0])->GetValue());
	}
	else
	{
		bestreg = choices[0]->Emit(build);
	}

	// Compare every choice. Better matches get copied to the bestreg.
	for (i = 1; i < choices.Size(); ++i)
	{
		ExpEmit checkreg = choices[i]->Emit(build);
		assert(checkreg.RegType == bestreg.RegType);
		build->Emit(opcode + checkreg.Konst, bestreg.RegNum, bestreg.RegNum, checkreg.RegNum);
		checkreg.Free(build);
	}
	return bestreg;
}

//==========================================================================
//
//
//
//==========================================================================
FxRandom::FxRandom(FRandom * r, FxExpression *mi, FxExpression *ma, const FScriptPosition &pos, bool nowarn)
: FxExpression(EFX_Random, pos)
{
	EmitTail = false;
	if (mi != nullptr && ma != nullptr)
	{
		min = new FxIntCast(mi, nowarn);
		max = new FxIntCast(ma, nowarn);
	}
	else min = max = nullptr;
	rng = r;
	ValueType = TypeSInt32;
}

//==========================================================================
//
//
//
//==========================================================================

FxRandom::~FxRandom()
{
	SAFE_DELETE(min);
	SAFE_DELETE(max);
}

//==========================================================================
//
//
//
//==========================================================================

PPrototype *FxRandom::ReturnProto()
{
	EmitTail = true;
	return FxExpression::ReturnProto();
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxRandom::Resolve(FCompileContext &ctx)
{
	CHECKRESOLVED();
	if (min && max)
	{
		RESOLVE(min, ctx);
		RESOLVE(max, ctx);
		ABORT(min && max);
		assert(min->ValueType == ValueType);
		assert(max->ValueType == ValueType);
	}
	return this;
};


//==========================================================================
//
//
//
//==========================================================================

int BuiltinRandom(VMValue *param, TArray<VMValue> &defaultparam, int numparam, VMReturn *ret, int numret)
{
	assert(numparam >= 1 && numparam <= 3);
	FRandom *rng = reinterpret_cast<FRandom *>(param[0].a);
	if (numparam == 1)
	{
		ACTION_RETURN_INT((*rng)());
	}
	else if (numparam == 2)
	{
		int maskval = param[1].i;
		ACTION_RETURN_INT(rng->Random2(maskval));
	}
	else if (numparam == 3)
	{
		int min = param[1].i, max = param[2].i;
		if (max < min)
		{
			swapvalues(max, min);
		}
		ACTION_RETURN_INT((*rng)(max - min + 1) + min);
	}

	// Shouldn't happen
	return 0;
}

ExpEmit FxRandom::Emit(VMFunctionBuilder *build)
{
	// Call DecoRandom to generate a random number.
	VMFunction *callfunc;
	PSymbol *sym = FindBuiltinFunction(NAME_BuiltinRandom, BuiltinRandom);

	assert(sym->IsKindOf(RUNTIME_CLASS(PSymbolVMFunction)));
	assert(((PSymbolVMFunction *)sym)->Function != nullptr);
	callfunc = ((PSymbolVMFunction *)sym)->Function;

	int opcode = (EmitTail ? OP_TAIL_K : OP_CALL_K);

	build->Emit(OP_PARAM, 0, REGT_POINTER | REGT_KONST, build->GetConstantAddress(rng, ATAG_RNG));
	if (min != nullptr && max != nullptr)
	{
		EmitParameter(build, min, ScriptPosition);
		EmitParameter(build, max, ScriptPosition);
		build->Emit(opcode, build->GetConstantAddress(callfunc, ATAG_OBJECT), 3, 1);
	}
	else
	{
		build->Emit(opcode, build->GetConstantAddress(callfunc, ATAG_OBJECT), 1, 1);
	}

	if (EmitTail)
	{
		ExpEmit call;
		call.Final = true;
		return call;
	}

	ExpEmit out(build, REGT_INT);
	build->Emit(OP_RESULT, 0, REGT_INT, out.RegNum);
	return out;
}

//==========================================================================
//
//
//
//==========================================================================
FxRandomPick::FxRandomPick(FRandom *r, TArray<FxExpression*> &expr, bool floaty, const FScriptPosition &pos, bool nowarn)
: FxExpression(EFX_RandomPick, pos)
{
	assert(expr.Size() > 0);
	choices.Resize(expr.Size());
	for (unsigned int index = 0; index < expr.Size(); index++)
	{
		if (floaty)
		{
			choices[index] = new FxFloatCast(expr[index]);
			expr[index] = nullptr;
		}
		else
		{
			choices[index] = new FxIntCast(expr[index], nowarn);
			expr[index] = nullptr;
		}

	}
	rng = r;
	if (floaty)
	{
		ValueType = TypeFloat64;
	}
	else
	{
		ValueType = TypeSInt32;
	}
}

//==========================================================================
//
//
//
//==========================================================================

FxRandomPick::~FxRandomPick()
{
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxRandomPick::Resolve(FCompileContext &ctx)
{
	CHECKRESOLVED();
	for (unsigned int index = 0; index < choices.Size(); index++)
	{
		RESOLVE(choices[index], ctx);
		ABORT(choices[index]);
		assert(choices[index]->ValueType == ValueType);
	}
	return this;
};


//==========================================================================
//
// FxPick :: Emit
//
// The expression:
//   a = pick[rng](i_0, i_1, i_2, ..., i_n)
//   [where i_x is a complete expression and not just a value]
// is syntactic sugar for:
//
//   switch(random[rng](0, n)) {
//     case 0: a = i_0;
//     case 1: a = i_1;
//     case 2: a = i_2;
//     ...
//     case n: a = i_n;
//   }
//
//==========================================================================

ExpEmit FxRandomPick::Emit(VMFunctionBuilder *build)
{
	unsigned i;

	assert(choices.Size() > 0);

	// Call BuiltinRandom to generate a random number.
	VMFunction *callfunc;
	PSymbol *sym = FindBuiltinFunction(NAME_BuiltinRandom, BuiltinRandom);

	assert(sym->IsKindOf(RUNTIME_CLASS(PSymbolVMFunction)));
	assert(((PSymbolVMFunction *)sym)->Function != nullptr);
	callfunc = ((PSymbolVMFunction *)sym)->Function;

	build->Emit(OP_PARAM, 0, REGT_POINTER | REGT_KONST, build->GetConstantAddress(rng, ATAG_RNG));
	build->EmitParamInt(0);
	build->EmitParamInt(choices.Size() - 1);
	build->Emit(OP_CALL_K, build->GetConstantAddress(callfunc, ATAG_OBJECT), 3, 1);

	ExpEmit resultreg(build, REGT_INT);
	build->Emit(OP_RESULT, 0, REGT_INT, resultreg.RegNum);
	build->Emit(OP_IJMP, resultreg.RegNum, 0);

	// Free the result register now. The simple code generation algorithm should
	// automatically pick it as the destination register for each case.
	resultreg.Free(build);

	// For floating point results, we need to get a new register, since we can't
	// reuse the integer one used to store the random result.
	if (ValueType->GetRegType() == REGT_FLOAT)
	{
		resultreg = ExpEmit(build, REGT_FLOAT);
		resultreg.Free(build);
	}

	// Allocate space for the jump table.
	size_t jumptable = build->Emit(OP_JMP, 0);
	for (i = 1; i < choices.Size(); ++i)
	{
		build->Emit(OP_JMP, 0);
	}

	// Emit each case
	TArray<size_t> finishes(choices.Size() - 1);
	for (unsigned i = 0; i < choices.Size(); ++i)
	{
		build->BackpatchToHere(jumptable + i);
		if (choices[i]->isConstant())
		{
			EmitLoad(build, resultreg, static_cast<FxConstant *>(choices[i])->GetValue());
		}
		else
		{
			ExpEmit casereg = choices[i]->Emit(build);
			if (casereg.RegNum != resultreg.RegNum)
			{ // The result of the case is in a different register from what
			  // was expected. Copy it to the one we wanted.

				resultreg.Reuse(build);	// This is really just for the assert in Reuse()
				build->Emit(ValueType->GetRegType() == REGT_INT ? OP_MOVE : OP_MOVEF, resultreg.RegNum, casereg.RegNum, 0);
				resultreg.Free(build);
			}
			// Free this register so the remaining cases can use it.
			casereg.Free(build);
		}
		// All but the final case needs a jump to the end of the expression's code.
		if (i + 1 < choices.Size())
		{
			size_t loc = build->Emit(OP_JMP, 0);
			finishes.Push(loc);
		}
	}
	// Backpatch each case (except the last, since it ends here) to jump to here.
	for (i = 0; i < choices.Size() - 1; ++i)
	{
		build->BackpatchToHere(finishes[i]);
	}
	// The result register needs to be in-use when we return.
	// It should have been freed earlier, so restore its in-use flag.
	resultreg.Reuse(build);
	choices.DeleteAndClear();
	choices.ShrinkToFit();
	return resultreg;
}

//==========================================================================
//
//
//
//==========================================================================
FxFRandom::FxFRandom(FRandom *r, FxExpression *mi, FxExpression *ma, const FScriptPosition &pos)
: FxRandom(r, nullptr, nullptr, pos, true)
{
	if (mi != nullptr && ma != nullptr)
	{
		min = new FxFloatCast(mi);
		max = new FxFloatCast(ma);
	}
	ValueType = TypeFloat64;
	ExprType = EFX_FRandom;
}

//==========================================================================
//
//
//
//==========================================================================

int BuiltinFRandom(VMValue *param, TArray<VMValue> &defaultparam, int numparam, VMReturn *ret, int numret)
{
	assert(numparam == 1 || numparam == 3);
	FRandom *rng = reinterpret_cast<FRandom *>(param[0].a);

	int random = (*rng)(0x40000000);
	double frandom = random / double(0x40000000);

	if (numparam == 3)
	{
		double min = param[1].f, max = param[2].f;
		if (max < min)
		{
			swapvalues(max, min);
		}
		ACTION_RETURN_FLOAT(frandom * (max - min) + min);
	}
	else
	{
		ACTION_RETURN_FLOAT(frandom);
	}
}

ExpEmit FxFRandom::Emit(VMFunctionBuilder *build)
{
	// Call the BuiltinFRandom function to generate a floating point random number..
	VMFunction *callfunc;
	PSymbol *sym = FindBuiltinFunction(NAME_BuiltinFRandom, BuiltinFRandom);

	assert(sym->IsKindOf(RUNTIME_CLASS(PSymbolVMFunction)));
	assert(((PSymbolVMFunction *)sym)->Function != nullptr);
	callfunc = ((PSymbolVMFunction *)sym)->Function;

	int opcode = (EmitTail ? OP_TAIL_K : OP_CALL_K);

	build->Emit(OP_PARAM, 0, REGT_POINTER | REGT_KONST, build->GetConstantAddress(rng, ATAG_RNG));
	if (min != nullptr && max != nullptr)
	{
		EmitParameter(build, min, ScriptPosition);
		EmitParameter(build, max, ScriptPosition);
		build->Emit(opcode, build->GetConstantAddress(callfunc, ATAG_OBJECT), 3, 1);
	}
	else
	{
		build->Emit(opcode, build->GetConstantAddress(callfunc, ATAG_OBJECT), 1, 1);
	}

	if (EmitTail)
	{
		ExpEmit call;
		call.Final = true;
		return call;
	}

	ExpEmit out(build, REGT_FLOAT);
	build->Emit(OP_RESULT, 0, REGT_FLOAT, out.RegNum);
	return out;
}

//==========================================================================
//
//
//
//==========================================================================

FxRandom2::FxRandom2(FRandom *r, FxExpression *m, const FScriptPosition &pos, bool nowarn)
: FxExpression(EFX_Random2, pos)
{
	EmitTail = false;
	rng = r;
	if (m) mask = new FxIntCast(m, nowarn);
	else mask = new FxConstant(-1, pos);
	ValueType = TypeSInt32;
}

//==========================================================================
//
//
//
//==========================================================================

FxRandom2::~FxRandom2()
{
	SAFE_DELETE(mask);
}

//==========================================================================
//
//
//
//==========================================================================

PPrototype *FxRandom2::ReturnProto()
{
	EmitTail = true;
	return FxExpression::ReturnProto();
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxRandom2::Resolve(FCompileContext &ctx)
{
	CHECKRESOLVED();
	SAFE_RESOLVE(mask, ctx);
	return this;
}

//==========================================================================
//
//
//
//==========================================================================

ExpEmit FxRandom2::Emit(VMFunctionBuilder *build)
{
	// Call the BuiltinRandom function to generate the random number.
	VMFunction *callfunc;
	PSymbol *sym = FindBuiltinFunction(NAME_BuiltinRandom, BuiltinRandom);

	assert(sym->IsKindOf(RUNTIME_CLASS(PSymbolVMFunction)));
	assert(((PSymbolVMFunction *)sym)->Function != nullptr);
	callfunc = ((PSymbolVMFunction *)sym)->Function;

	int opcode = (EmitTail ? OP_TAIL_K : OP_CALL_K);

	build->Emit(OP_PARAM, 0, REGT_POINTER | REGT_KONST, build->GetConstantAddress(rng, ATAG_RNG));
	EmitParameter(build, mask, ScriptPosition);
	build->Emit(opcode, build->GetConstantAddress(callfunc, ATAG_OBJECT), 2, 1);

	if (EmitTail)
	{
		ExpEmit call;
		call.Final = true;
		return call;
	}

	ExpEmit out(build, REGT_INT);
	build->Emit(OP_RESULT, 0, REGT_INT, out.RegNum);
	return out;
}

//==========================================================================
//
//
//
//==========================================================================

FxIdentifier::FxIdentifier(FName name, const FScriptPosition &pos)
: FxExpression(EFX_Identifier, pos)
{
	Identifier = name;
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxIdentifier::Resolve(FCompileContext& ctx)
{
	PSymbol * sym;
	FxExpression *newex = nullptr;
	int num;
	
	CHECKRESOLVED();

	// Local variables have highest priority.
	FxLocalVariableDeclaration *local = ctx.FindLocalVariable(Identifier);
	if (local != nullptr)
	{
		if (local->ExprType == EFX_StaticArray)
		{
			auto x = new FxStaticArrayVariable(local, ScriptPosition);
			delete this;
			return x->Resolve(ctx);
		}
		else if (local->ValueType->GetRegType() != REGT_NIL)
		{
			auto x = new FxLocalVariable(local, ScriptPosition);
			delete this;
			return x->Resolve(ctx);
		}
		else
		{
			auto x = new FxStackVariable(local->ValueType, local->StackOffset, ScriptPosition);
			delete this;
			return x->Resolve(ctx);
		}
	}

	if (Identifier == NAME_Default)
	{
		if (ctx.Function->Variants[0].SelfClass == nullptr)
		{
			ScriptPosition.Message(MSG_ERROR, "Unable to access class defaults from static function");
			delete this;
			return nullptr;
		}
		if (!ctx.Function->Variants[0].SelfClass->IsKindOf(RUNTIME_CLASS(PClassActor)))
		{
			ScriptPosition.Message(MSG_ERROR, "'Default' requires an actor type.");
			delete this;
			return nullptr;
		}

		FxExpression * x = new FxClassDefaults(new FxSelf(ScriptPosition), ScriptPosition);
		delete this;
		return x->Resolve(ctx);
	}

	// Ugh, the horror. Constants need to be taken from the owning class, but members from the self class to catch invalid accesses here...
	// see if the current class (if valid) defines something with this name.
	PSymbolTable *symtbl;

	// first check fields in self
	if ((sym = ctx.FindInSelfClass(Identifier, symtbl)) != nullptr)
	{
		if (sym->IsKindOf(RUNTIME_CLASS(PField)))
		{
			FxExpression *self = new FxSelf(ScriptPosition);
			self = self->Resolve(ctx);
			newex = ResolveMember(ctx, ctx.Function->Variants[0].SelfClass, self, ctx.Function->Variants[0].SelfClass);
			ABORT(newex);
			goto foundit;
		}
	}

	// now check in the owning class.
	if (newex == nullptr && (sym = ctx.FindInClass(Identifier, symtbl)) != nullptr)
	{
		if (sym->IsKindOf(RUNTIME_CLASS(PSymbolConst)))
		{
			ScriptPosition.Message(MSG_DEBUGLOG, "Resolving name '%s' as class constant\n", Identifier.GetChars());
			newex = FxConstant::MakeConstant(sym, ScriptPosition);
			goto foundit;
		}
		// Do this check for ZScript as well, so that a clearer error message can be printed. MSG_OPTERROR will default to MSG_ERROR there.
		else if (ctx.Function->Variants[0].SelfClass != ctx.Class && sym->IsKindOf(RUNTIME_CLASS(PField)))
		{
			FxExpression *self = new FxSelf(ScriptPosition, true);
			self = self->Resolve(ctx);
			newex = ResolveMember(ctx, ctx.Class, self, ctx.Class);
			ABORT(newex);
			ScriptPosition.Message(MSG_OPTERROR, "Self pointer used in ambiguous context; VM execution may abort!");
			ctx.Unsafe = true;
			goto foundit;
		}
		else
		{
			if (sym->IsKindOf(RUNTIME_CLASS(PFunction)))
			{
				ScriptPosition.Message(MSG_ERROR, "Function '%s' used without ().\n", Identifier.GetChars());
			}
			else
			{
				ScriptPosition.Message(MSG_ERROR, "Invalid member identifier '%s'.\n", Identifier.GetChars());
			}
			delete this;
			return nullptr;
		}
	}

	if (noglobal)
	{
		// This is needed to properly resolve class names on the left side of the member access operator
		ValueType = TypeError;
		return this;
	}

	// now check the global identifiers.
	if (newex == nullptr && (sym = ctx.FindGlobal(Identifier)) != nullptr)
	{
		if (sym->IsKindOf(RUNTIME_CLASS(PSymbolConst)))
		{
			ScriptPosition.Message(MSG_DEBUGLOG, "Resolving name '%s' as global constant\n", Identifier.GetChars());
			newex = FxConstant::MakeConstant(sym, ScriptPosition);
			goto foundit;
		}
		else if (sym->IsKindOf(RUNTIME_CLASS(PField)))
		{
			// internally defined global variable
			ScriptPosition.Message(MSG_DEBUGLOG, "Resolving name '%s' as global variable\n", Identifier.GetChars());
			newex = new FxGlobalVariable(static_cast<PField *>(sym), ScriptPosition);
			goto foundit;
		}
		else
		{
			ScriptPosition.Message(MSG_ERROR, "Invalid global identifier '%s'\n", Identifier.GetChars());
			delete this;
			return nullptr;
		}
	}

	// and line specials
	if (newex == nullptr && (num = P_FindLineSpecial(Identifier, nullptr, nullptr)))
	{
		ScriptPosition.Message(MSG_DEBUGLOG, "Resolving name '%s' as line special %d\n", Identifier.GetChars(), num);
		newex = new FxConstant(num, ScriptPosition);
		goto foundit;
	}

	auto cvar = FindCVar(Identifier.GetChars(), nullptr);
	if (cvar != nullptr)
	{
		if (cvar->GetFlags() & CVAR_USERINFO)
		{
			ScriptPosition.Message(MSG_ERROR, "Cannot access userinfo CVARs directly. Use GetCVar() instead.");
			delete this;
			return nullptr;
		}
		newex = new FxCVar(cvar, ScriptPosition);
		goto foundit;
	}
	
	ScriptPosition.Message(MSG_ERROR, "Unknown identifier '%s'", Identifier.GetChars());
	delete this;
	return nullptr;

foundit:
	delete this;
	return newex? newex->Resolve(ctx) : nullptr;
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxIdentifier::ResolveMember(FCompileContext &ctx, PStruct *classctx, FxExpression *&object, PStruct *objtype)
{
	PSymbol *sym;
	PSymbolTable *symtbl;
	bool isclass = objtype->IsKindOf(RUNTIME_CLASS(PClass));

	if (Identifier == NAME_Default)
	{
		if (!objtype->IsKindOf(RUNTIME_CLASS(PClassActor)))
		{
			ScriptPosition.Message(MSG_ERROR, "'Default' requires an actor type.");
			delete this;
			return nullptr;
		}

		FxExpression * x = new FxClassDefaults(object, ScriptPosition);
		object = nullptr;
		delete this;
		return x->Resolve(ctx);
	}

	if ((sym = objtype->Symbols.FindSymbolInTable(Identifier, symtbl)) != nullptr)
	{
		if (sym->IsKindOf(RUNTIME_CLASS(PSymbolConst)))
		{
			ScriptPosition.Message(MSG_DEBUGLOG, "Resolving name '%s' as %s constant\n", Identifier.GetChars(), isclass ? "class" : "struct");
			delete object;
			object = nullptr;
			return FxConstant::MakeConstant(sym, ScriptPosition);
		}
		else if (sym->IsKindOf(RUNTIME_CLASS(PField)))
		{
			PField *vsym = static_cast<PField*>(sym);

			// We have 4 cases to consider here:
			// 1. The symbol is a static/meta member (not implemented yet) which is always accessible.
			// 2. This is a static function 
			// 3. This is an action function with a restricted self pointer
			// 4. This is a normal member or unrestricted action function.
			if (vsym->Flags & VARF_Deprecated && !ctx.FromDecorate)
			{
				ScriptPosition.Message(MSG_WARNING, "Accessing deprecated member variable %s", vsym->SymbolName.GetChars());
			}
			if ((vsym->Flags & VARF_Private) && symtbl != &classctx->Symbols)
			{
				ScriptPosition.Message(MSG_ERROR, "Private member %s not accessible", vsym->SymbolName.GetChars());
				return nullptr;
			}

			auto x = isclass ? new FxClassMember(object, vsym, ScriptPosition) : new FxStructMember(object, vsym, ScriptPosition);
			object = nullptr;
			return x->Resolve(ctx);
		}
		else
		{
			if (sym->IsKindOf(RUNTIME_CLASS(PFunction)))
			{
				ScriptPosition.Message(MSG_ERROR, "Function '%s' used without ().\n", Identifier.GetChars());
			}
			else
			{
				ScriptPosition.Message(MSG_ERROR, "Invalid member identifier '%s'.\n", Identifier.GetChars());
			}
			delete object;
			object = nullptr;
			return nullptr;
		}
	}
	else
	{
		ScriptPosition.Message(MSG_ERROR, "Unknown identifier '%s'", Identifier.GetChars());
		delete object;
		object = nullptr;
		return nullptr;
	}
}

//==========================================================================
//
//
//
//==========================================================================

FxMemberIdentifier::FxMemberIdentifier(FxExpression *left, FName name, const FScriptPosition &pos)
	: FxIdentifier(name, pos)
{
	Object = left;
	ExprType = EFX_MemberIdentifier;
}

FxMemberIdentifier::~FxMemberIdentifier()
{
	SAFE_DELETE(Object);
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxMemberIdentifier::Resolve(FCompileContext& ctx)
{
	PStruct *ccls = nullptr;
	CHECKRESOLVED();

	if (Object->ExprType == EFX_Identifier)
	{
		// If the left side is a class name for a static member function call it needs to be resolved manually 
		// because the resulting value type would cause problems in nearly every other place where identifiers are being used.
		ccls = FindStructType(static_cast<FxIdentifier *>(Object)->Identifier);
		if (ccls != nullptr) static_cast<FxIdentifier *>(Object)->noglobal = true;
	}

	SAFE_RESOLVE(Object, ctx);

	if (Identifier == FName("allmap"))
	{
		int a = 2;
	}

	// check for class or struct constants if the left side is a type name.
	if (Object->ValueType == TypeError)
	{
		if (ccls != nullptr)
		{
			if (!ccls->IsKindOf(RUNTIME_CLASS(PClass)) || static_cast<PClass *>(ccls)->bExported)
			{
				PSymbol *sym;
				if ((sym = ccls->Symbols.FindSymbol(Identifier, true)) != nullptr)
				{
					if (sym->IsKindOf(RUNTIME_CLASS(PSymbolConst)))
					{
						ScriptPosition.Message(MSG_DEBUGLOG, "Resolving name '%s.%s' as constant\n", ccls->TypeName.GetChars(), Identifier.GetChars());
						delete this;
						return FxConstant::MakeConstant(sym, ScriptPosition);
					}
					else
					{
						ScriptPosition.Message(MSG_ERROR, "Unable to access '%s.%s' in a static context\n", ccls->TypeName.GetChars(), Identifier.GetChars());
						delete this;
						return nullptr;
					}
				}
			}
		}
	}

	// allow accessing the color channels by mapping the type to a matching struct which defines them.
	if (Object->ValueType == TypeColor)
	{
		Object->ValueType = TypeColorStruct;
	}

	else if (Object->ValueType->IsKindOf(RUNTIME_CLASS(PPointer)))
	{
		auto ptype = static_cast<PPointer *>(Object->ValueType)->PointedType;
		if (ptype->IsKindOf(RUNTIME_CLASS(PStruct)))
		{
			auto ret = ResolveMember(ctx, ctx.Class, Object, static_cast<PStruct *>(ptype));
			delete this;
			return ret;
		}
	}
	else if (Object->ValueType->IsKindOf(RUNTIME_CLASS(PStruct)))
	{
		auto ret = ResolveMember(ctx, ctx.Class, Object, static_cast<PStruct *>(Object->ValueType));
		delete this;
		return ret;
	}

	ScriptPosition.Message(MSG_ERROR, "Left side of %s is not a struct or class", Identifier.GetChars());
	delete this;
	return nullptr;
}

//==========================================================================
//
//
//
//==========================================================================

FxLocalVariable::FxLocalVariable(FxLocalVariableDeclaration *var, const FScriptPosition &sc)
	: FxExpression(EFX_LocalVariable, sc)
{
	Variable = var;
	ValueType = var->ValueType;
	AddressRequested = false;
	RegOffset = 0;
}

FxExpression *FxLocalVariable::Resolve(FCompileContext &ctx)
{
	CHECKRESOLVED();
	return this;
}

bool FxLocalVariable::RequestAddress(FCompileContext &ctx, bool *writable)
{
	AddressRequested = true;
	if (writable != nullptr) *writable = !ctx.CheckReadOnly(Variable->VarFlags);
	return true;
}
	
ExpEmit FxLocalVariable::Emit(VMFunctionBuilder *build)
{
	// 'Out' variables are actually pointers but this fact must be hidden to the script.
	if (Variable->VarFlags & VARF_Out)
	{
		if (!AddressRequested)
		{
			ExpEmit reg(build, ValueType->GetRegType(), ValueType->GetRegCount());
			build->Emit(ValueType->GetLoadOp(), reg.RegNum, Variable->RegNum, build->GetConstantInt(RegOffset));
			return reg;
		}
		else
		{
			if (RegOffset == 0) return ExpEmit(Variable->RegNum, REGT_POINTER, false, true);
			ExpEmit reg(build, REGT_POINTER);
			build->Emit(OP_ADDA_RK, reg.RegNum, Variable->RegNum, build->GetConstantInt(RegOffset));
			return reg;
		}
	}
	else
	{
		ExpEmit ret(Variable->RegNum + RegOffset, Variable->ValueType->GetRegType(), false, true);
		ret.RegCount = ValueType->GetRegCount();
		if (AddressRequested) ret.Target = true;
		return ret;
	}
}


//==========================================================================
//
//
//
//==========================================================================

FxStaticArrayVariable::FxStaticArrayVariable(FxLocalVariableDeclaration *var, const FScriptPosition &sc)
	: FxExpression(EFX_StaticArrayVariable, sc)
{
	Variable = static_cast<FxStaticArray*>(var);
	ValueType = Variable->ValueType;
}

FxExpression *FxStaticArrayVariable::Resolve(FCompileContext &ctx)
{
	CHECKRESOLVED();
	return this;
}

bool FxStaticArrayVariable::RequestAddress(FCompileContext &ctx, bool *writable)
{
	AddressRequested = true;
	if (writable != nullptr) *writable = false;
	return true;
}

ExpEmit FxStaticArrayVariable::Emit(VMFunctionBuilder *build)
{
	// returns the first const register for this array
	return ExpEmit(Variable->StackOffset, Variable->ElementType->GetRegType(), true, false);
}


//==========================================================================
//
//
//
//==========================================================================

FxSelf::FxSelf(const FScriptPosition &pos, bool deccheck)
: FxExpression(EFX_Self, pos)
{
	check = deccheck;
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxSelf::Resolve(FCompileContext& ctx)
{
	CHECKRESOLVED();
	if (ctx.Function == nullptr || ctx.Function->Variants[0].SelfClass == nullptr)
	{
		ScriptPosition.Message(MSG_ERROR, "self used outside of a member function");
		delete this;
		return nullptr;
	}
	ValueType = NewPointer(ctx.Function->Variants[0].SelfClass);
	return this;
}  

//==========================================================================
//
//
//
//==========================================================================

ExpEmit FxSelf::Emit(VMFunctionBuilder *build)
{
	if (check)
	{
		build->Emit(OP_EQA_R, 1, 0, 1);
		build->Emit(OP_JMP, 1);
		build->Emit(OP_THROW, 2, X_BAD_SELF);
	}
	// self is always the first pointer passed to the function
	return ExpEmit(0, REGT_POINTER, false, true);
}


//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxSuper::Resolve(FCompileContext& ctx)
{
	CHECKRESOLVED();
	if (ctx.Function == nullptr || ctx.Function->Variants[0].SelfClass == nullptr)
	{
		ScriptPosition.Message(MSG_ERROR, "super used outside of a member function");
		delete this;
		return nullptr;
	}
	ValueType = TypeError;	// this intentionally resolves to an invalid type so that it cannot be used outside of super calls.
	return this;
}

//==========================================================================
//
//
//
//==========================================================================

FxClassDefaults::FxClassDefaults(FxExpression *X, const FScriptPosition &pos)
	: FxExpression(EFX_ClassDefaults, pos)
{
	obj = X;
	EmitTail = false;
}

FxClassDefaults::~FxClassDefaults()
{
	SAFE_DELETE(obj);
}


//==========================================================================
//
//
//
//==========================================================================

PPrototype *FxClassDefaults::ReturnProto()
{
	EmitTail = true;
	return FxExpression::ReturnProto();
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxClassDefaults::Resolve(FCompileContext& ctx)
{
	CHECKRESOLVED();
	SAFE_RESOLVE(obj, ctx);
	assert(obj->ValueType->IsKindOf(RUNTIME_CLASS(PPointer)));
	ValueType = NewPointer(static_cast<PPointer*>(obj->ValueType)->PointedType, true);
	return this;
}

//==========================================================================
//
//
//
//==========================================================================

ExpEmit FxClassDefaults::Emit(VMFunctionBuilder *build)
{
	ExpEmit ob = obj->Emit(build);
	ob.Free(build);
	ExpEmit meta(build, REGT_POINTER);
	build->Emit(OP_META, meta.RegNum, ob.RegNum);
	build->Emit(OP_LO, meta.RegNum, meta.RegNum, build->GetConstantInt(myoffsetof(PClass, Defaults)));
	return meta;

}

//==========================================================================
//
//
//
//==========================================================================

FxGlobalVariable::FxGlobalVariable(PField* mem, const FScriptPosition &pos)
	: FxExpression(EFX_GlobalVariable, pos)
{
	membervar = mem;
	AddressRequested = false;
	AddressWritable = true;	// must be true unless classx tells us otherwise if requested.
}

//==========================================================================
//
//
//
//==========================================================================

bool FxGlobalVariable::RequestAddress(FCompileContext &ctx, bool *writable)
{
	AddressRequested = true;
	if (writable != nullptr) *writable = AddressWritable && !ctx.CheckReadOnly(membervar->Flags);
	return true;
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxGlobalVariable::Resolve(FCompileContext &ctx)
{
	CHECKRESOLVED();
	ValueType = membervar->Type;
	return this;
}

ExpEmit FxGlobalVariable::Emit(VMFunctionBuilder *build)
{
	ExpEmit obj(build, REGT_POINTER);

	build->Emit(OP_LKP, obj.RegNum, build->GetConstantAddress((void*)(intptr_t)membervar->Offset, ATAG_GENERIC));
	if (AddressRequested)
	{
		return obj;
	}

	ExpEmit loc(build, membervar->Type->GetRegType(), membervar->Type->GetRegCount());

	if (membervar->BitValue == -1)
	{
		int offsetreg = build->GetConstantInt(0);
		build->Emit(membervar->Type->GetLoadOp(), loc.RegNum, obj.RegNum, offsetreg);
	}
	else
	{
		build->Emit(OP_LBIT, loc.RegNum, obj.RegNum, 1 << membervar->BitValue);
	}
	obj.Free(build);
	return loc;
}


//==========================================================================
//
//
//
//==========================================================================

FxCVar::FxCVar(FBaseCVar *cvar, const FScriptPosition &pos)
	: FxExpression(EFX_CVar, pos)
{
	CVar = cvar;
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxCVar::Resolve(FCompileContext &ctx)
{
	CHECKRESOLVED();
	switch (CVar->GetRealType())
	{
	case CVAR_Bool:
	case CVAR_DummyBool:
		ValueType = TypeBool;
		break;

	case CVAR_Int:
	case CVAR_DummyInt:
		ValueType = TypeSInt32;
		break;

	case CVAR_Color:
		ValueType = TypeColor;
		break;

	case CVAR_Float:
		ValueType = TypeFloat64;
		break;

	case CVAR_String:
		ValueType = TypeString;
		break;

	default:
		ScriptPosition.Message(MSG_ERROR, "Unknown CVar type for %s", CVar->GetName());
		delete this;
		return nullptr;
	}
	return this;
}

ExpEmit FxCVar::Emit(VMFunctionBuilder *build)
{
	ExpEmit dest(build, ValueType->GetRegType());
	ExpEmit addr(build, REGT_POINTER);
	int nul = build->GetConstantInt(0);
	switch (CVar->GetRealType())
	{
	case CVAR_Int:
		build->Emit(OP_LKP, addr.RegNum, build->GetConstantAddress(&static_cast<FIntCVar *>(CVar)->Value, ATAG_GENERIC));
		build->Emit(OP_LW, dest.RegNum, addr.RegNum, nul);
		break;

	case CVAR_Color:
		build->Emit(OP_LKP, addr.RegNum, build->GetConstantAddress(&static_cast<FColorCVar *>(CVar)->Value, ATAG_GENERIC));
		build->Emit(OP_LW, dest.RegNum, addr.RegNum, nul);
		break;

	case CVAR_Float:
		build->Emit(OP_LKP, addr.RegNum, build->GetConstantAddress(&static_cast<FFloatCVar *>(CVar)->Value, ATAG_GENERIC));
		build->Emit(OP_LSP, dest.RegNum, addr.RegNum, nul);
		break;

	case CVAR_Bool:
		build->Emit(OP_LKP, addr.RegNum, build->GetConstantAddress(&static_cast<FBoolCVar *>(CVar)->Value, ATAG_GENERIC));
		build->Emit(OP_LBU, dest.RegNum, addr.RegNum, nul);
		break;

	case CVAR_String:
		build->Emit(OP_LKP, addr.RegNum, build->GetConstantAddress(&static_cast<FStringCVar *>(CVar)->Value, ATAG_GENERIC));
		build->Emit(OP_LS, dest.RegNum, addr.RegNum, nul);
		break;

	case CVAR_DummyBool:
	{
		auto cv = static_cast<FFlagCVar *>(CVar);
		build->Emit(OP_LKP, addr.RegNum, build->GetConstantAddress(&cv->ValueVar.Value, ATAG_GENERIC));
		build->Emit(OP_LW, dest.RegNum, addr.RegNum, nul);
		build->Emit(OP_SRL_RI, dest.RegNum, dest.RegNum, cv->BitNum);
		build->Emit(OP_AND_RK, dest.RegNum, dest.RegNum, build->GetConstantInt(1));
		break;
	}

	case CVAR_DummyInt:
	{
		auto cv = static_cast<FMaskCVar *>(CVar);
		build->Emit(OP_LKP, addr.RegNum, build->GetConstantAddress(&cv->ValueVar.Value, ATAG_GENERIC));
		build->Emit(OP_LW, dest.RegNum, addr.RegNum, nul);
		build->Emit(OP_AND_RK, dest.RegNum, dest.RegNum, build->GetConstantInt(cv->BitVal));
		build->Emit(OP_SRL_RI, dest.RegNum, dest.RegNum, cv->BitNum);
		break;
	}

	default:
		assert(false && "Unsupported CVar type");
		break;
	}
	addr.Free(build);
	return dest;
}


//==========================================================================
//
//
//
//==========================================================================

FxStackVariable::FxStackVariable(PType *type, int offset, const FScriptPosition &pos)
	: FxExpression(EFX_StackVariable, pos)
{
	membervar = new PField(NAME_None, type, 0, offset);
	AddressRequested = false;
	AddressWritable = true;	// must be true unless classx tells us otherwise if requested.
}

//==========================================================================
//
// force delete the PField because we know we won't need it anymore
// and it won't get GC'd until the compiler finishes.
//
//==========================================================================

FxStackVariable::~FxStackVariable()
{
	// Q: Is this good or bad? Needs testing if this is fine or better left to the GC anyway. DObject's destructor is anything but cheap.
	membervar->ObjectFlags |= OF_YesReallyDelete;
	delete membervar;
}

//==========================================================================
//
//
//==========================================================================

void FxStackVariable::ReplaceField(PField *newfield)
{
	membervar->ObjectFlags |= OF_YesReallyDelete;
	delete membervar;
	membervar = newfield;
}

//==========================================================================
//
//
//
//==========================================================================

bool FxStackVariable::RequestAddress(FCompileContext &ctx, bool *writable)
{
	AddressRequested = true;
	if (writable != nullptr) *writable = AddressWritable && !ctx.CheckReadOnly(membervar->Flags);
	return true;
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxStackVariable::Resolve(FCompileContext &ctx)
{
	CHECKRESOLVED();
	ValueType = membervar->Type;
	return this;
}

ExpEmit FxStackVariable::Emit(VMFunctionBuilder *build)
{
	int offsetreg = -1;
	
	if (membervar->Offset != 0) offsetreg = build->GetConstantInt((int)membervar->Offset);

	if (AddressRequested)
	{
		if (offsetreg >= 0)
		{
			ExpEmit obj(build, REGT_POINTER);
			build->Emit(OP_ADDA_RK, obj.RegNum, build->FramePointer.RegNum, offsetreg);
			return obj;
		}
		else
		{
			return build->FramePointer;
		}
	}
	ExpEmit loc(build, membervar->Type->GetRegType(), membervar->Type->GetRegCount());

	if (membervar->BitValue == -1)
	{
		if (offsetreg == -1) offsetreg = build->GetConstantInt(0);
		build->Emit(membervar->Type->GetLoadOp(), loc.RegNum, build->FramePointer.RegNum, offsetreg);
	}
	else
	{
		ExpEmit obj(build, REGT_POINTER);
		if (offsetreg >= 0) build->Emit(OP_ADDA_RK, obj.RegNum, build->FramePointer.RegNum, offsetreg);
		obj.Free(build);
		build->Emit(OP_LBIT, loc.RegNum, obj.RegNum, 1 << membervar->BitValue);
	}
	return loc;
}


//==========================================================================
//
//
//
//==========================================================================

FxStructMember::FxStructMember(FxExpression *x, PField* mem, const FScriptPosition &pos)
	: FxExpression(EFX_StructMember, pos)
{
	classx = x;
	membervar = mem;
	AddressRequested = false;
	AddressWritable = true;	// must be true unless classx tells us otherwise if requested.
}

//==========================================================================
//
//
//
//==========================================================================

FxStructMember::~FxStructMember()
{
	SAFE_DELETE(classx);
}

//==========================================================================
//
//
//
//==========================================================================

bool FxStructMember::RequestAddress(FCompileContext &ctx, bool *writable)
{
	// Cannot take the address of metadata variables.
	if (membervar->Flags & VARF_Static)
	{
		return false;
	}
	AddressRequested = true;
	if (writable != nullptr) *writable = (AddressWritable && !ctx.CheckReadOnly(membervar->Flags) &&
											(!classx->ValueType->IsKindOf(RUNTIME_CLASS(PPointer)) || !static_cast<PPointer*>(classx->ValueType)->IsConst));
	return true;
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxStructMember::Resolve(FCompileContext &ctx)
{
	CHECKRESOLVED();
	SAFE_RESOLVE(classx, ctx);

	if (membervar->SymbolName == NAME_Default)
	{
		if (!classx->ValueType->IsKindOf(RUNTIME_CLASS(PPointer))
			|| !static_cast<PPointer *>(classx->ValueType)->PointedType->IsKindOf(RUNTIME_CLASS(AActor)))
		{
			ScriptPosition.Message(MSG_ERROR, "'Default' requires an actor type.");
			delete this;
			return nullptr;
		}
		FxExpression * x = new FxClassDefaults(classx, ScriptPosition);
		classx = nullptr;
		delete this;
		return x->Resolve(ctx);
	}

	if (classx->ValueType->IsKindOf(RUNTIME_CLASS(PPointer)))
	{
		PPointer *ptrtype = dyn_cast<PPointer>(classx->ValueType);
		if (ptrtype == nullptr || !ptrtype->PointedType->IsKindOf(RUNTIME_CLASS(PStruct)))
		{
			ScriptPosition.Message(MSG_ERROR, "Member variable requires a struct or class object.");
			delete this;
			return nullptr;
		}
	}
	else if (classx->ValueType->IsKindOf(RUNTIME_CLASS(PStruct)))
	{
		// if this is a struct within a class or another struct we can simplify the expression by creating a new PField with a cumulative offset.
		if (classx->ExprType == EFX_ClassMember || classx->ExprType == EFX_StructMember)
		{
			auto parentfield = static_cast<FxStructMember *>(classx)->membervar;
			// PFields are garbage collected so this will be automatically taken care of later.
			auto newfield = new PField(membervar->SymbolName, membervar->Type, membervar->Flags | parentfield->Flags, membervar->Offset + parentfield->Offset);
			newfield->BitValue = membervar->BitValue;
			static_cast<FxStructMember *>(classx)->membervar = newfield;
			classx->isresolved = false;	// re-resolve the parent so it can also check if it can be optimized away.
			auto x = classx->Resolve(ctx);
			classx = nullptr;
			return x;
		}
		else if (classx->ExprType == EFX_GlobalVariable)
		{
			auto parentfield = static_cast<FxGlobalVariable *>(classx)->membervar;
			auto newfield = new PField(membervar->SymbolName, membervar->Type, membervar->Flags | parentfield->Flags, membervar->Offset + parentfield->Offset);
			newfield->BitValue = membervar->BitValue;
			static_cast<FxGlobalVariable *>(classx)->membervar = newfield;
			classx->isresolved = false;	// re-resolve the parent so it can also check if it can be optimized away.
			auto x = classx->Resolve(ctx);
			classx = nullptr;
			return x;
		}
		else if (classx->ExprType == EFX_StackVariable)
		{
			auto parentfield = static_cast<FxStackVariable *>(classx)->membervar;
			auto newfield = new PField(membervar->SymbolName, membervar->Type, membervar->Flags | parentfield->Flags, membervar->Offset + parentfield->Offset);
			newfield->BitValue = membervar->BitValue;
			static_cast<FxStackVariable *>(classx)->ReplaceField(newfield);
			classx->isresolved = false;	// re-resolve the parent so it can also check if it can be optimized away.
			auto x = classx->Resolve(ctx);
			classx = nullptr;
			return x;
		}
		else if (classx->ExprType == EFX_LocalVariable && classx->IsVector())	// vectors are a special case because they are held in registers
		{
			// since this is a vector, all potential things that may get here are single float or an xy-vector.
			auto locvar = static_cast<FxLocalVariable *>(classx);
			locvar->RegOffset = int(membervar->Offset / 8);
			locvar->ValueType = membervar->Type;
			classx = nullptr;
			delete this;
			return locvar;
		}
		else if (classx->ExprType == EFX_LocalVariable && classx->ValueType == TypeColorStruct)
		{
			// This needs special treatment because it'd require accessing the register via address.
			// Fortunately this is the only place where this kind of access is ever needed so an explicit handling is acceptable.
			int bits;
			switch (membervar->SymbolName.GetIndex())
			{
			case NAME_a: bits = 24; break;
			case NAME_r: bits = 16; break;
			case NAME_g: bits = 8; break;
			case NAME_b: default: bits = 0; break;
			}
			classx->ValueType = TypeColor;	// need to set it back.
			FxExpression *x = classx;
			if (bits > 0) x = new FxShift(TK_URShift, x, new FxConstant(bits, ScriptPosition));
			x = new FxBitOp('&', x, new FxConstant(255, ScriptPosition));
			classx = nullptr;
			delete this;
			return x->Resolve(ctx);
		}
		else
		{
			if (!(classx->RequestAddress(ctx, &AddressWritable)))
			{
				ScriptPosition.Message(MSG_ERROR, "unable to dereference left side of %s", membervar->SymbolName.GetChars());
				delete this;
				return nullptr;
			}
		}
	}
	ValueType = membervar->Type;
	return this;
}

ExpEmit FxStructMember::Emit(VMFunctionBuilder *build)
{
	ExpEmit obj = classx->Emit(build);
	assert(obj.RegType == REGT_POINTER);

	if (obj.Konst)
	{
		// If the situation where we are dereferencing a constant
		// pointer is common, then it would probably be worthwhile
		// to add new opcodes for those. But as of right now, I
		// don't expect it to be a particularly common case.
		ExpEmit newobj(build, REGT_POINTER);
		build->Emit(OP_LKP, newobj.RegNum, obj.RegNum);
		obj = newobj;
	}

	if (membervar->Flags & VARF_Static)
	{
		obj.Free(build);
		ExpEmit meta(build, REGT_POINTER);
		build->Emit(OP_META, meta.RegNum, obj.RegNum);
		obj = meta;
	}

	if (AddressRequested)
	{
		if (membervar->Offset == 0)
		{
			return obj;
		}
		obj.Free(build);
		ExpEmit out(build, REGT_POINTER);
		build->Emit(OP_ADDA_RK, out.RegNum, obj.RegNum, build->GetConstantInt((int)membervar->Offset));
		return out;
	}

	int offsetreg = build->GetConstantInt((int)membervar->Offset);
	ExpEmit loc(build, membervar->Type->GetRegType(), membervar->Type->GetRegCount());

	if (membervar->BitValue == -1)
	{
		build->Emit(membervar->Type->GetLoadOp(), loc.RegNum, obj.RegNum, offsetreg);
	}
	else
	{
		ExpEmit out(build, REGT_POINTER);
		build->Emit(OP_ADDA_RK, out.RegNum, obj.RegNum, offsetreg);
		build->Emit(OP_LBIT, loc.RegNum, out.RegNum, 1 << membervar->BitValue);
		out.Free(build);
	}
	obj.Free(build);
	return loc;
}


//==========================================================================
//
// not really needed at the moment but may become useful with meta properties
// and some other class-specific extensions.
//
//==========================================================================

FxClassMember::FxClassMember(FxExpression *x, PField* mem, const FScriptPosition &pos)
: FxStructMember(x, mem, pos)
{
	ExprType = EFX_ClassMember;
}

//==========================================================================
//
//
//
//==========================================================================

FxArrayElement::FxArrayElement(FxExpression *base, FxExpression *_index)
:FxExpression(EFX_ArrayElement, base->ScriptPosition)
{
	Array=base;
	index = _index;
	AddressRequested = false;
	AddressWritable = false;
}

//==========================================================================
//
//
//
//==========================================================================

FxArrayElement::~FxArrayElement()
{
	SAFE_DELETE(Array);
	SAFE_DELETE(index);
}

//==========================================================================
//
//
//
//==========================================================================

bool FxArrayElement::RequestAddress(FCompileContext &ctx, bool *writable)
{
	AddressRequested = true;
	if (writable != nullptr) *writable = AddressWritable;
	return true;
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxArrayElement::Resolve(FCompileContext &ctx)
{
	CHECKRESOLVED();
	SAFE_RESOLVE(Array,ctx);
	SAFE_RESOLVE(index,ctx);

	if (index->ValueType->GetRegType() == REGT_FLOAT /* lax */)
	{
		// DECORATE allows floats here so cast them to int.
		index = new FxIntCast(index, ctx.FromDecorate);
		index = index->Resolve(ctx);
		if (index == nullptr) 
		{
			delete this;
			return nullptr;
		}
	}
	if (!index->IsInteger())
	{
		ScriptPosition.Message(MSG_ERROR, "Array index must be integer");
		delete this;
		return nullptr;
	}

	PArray *arraytype = dyn_cast<PArray>(Array->ValueType);
	if (arraytype == nullptr)
	{
		// Check if we got a pointer to an array. Some native data structures (like the line list in sectors) use this.
		PPointer *ptype = dyn_cast<PPointer>(Array->ValueType);
		if (ptype == nullptr || !ptype->PointedType->IsKindOf(RUNTIME_CLASS(PArray)))
		{
			ScriptPosition.Message(MSG_ERROR, "'[]' can only be used with arrays.");
			delete this;
			return nullptr;
		}
		arraytype = static_cast<PArray*>(ptype->PointedType);
		arrayispointer = true;
	}
	if (index->isConstant())
	{
		unsigned indexval = static_cast<FxConstant *>(index)->GetValue().GetInt();
		if (indexval >= arraytype->ElementCount)
		{
			ScriptPosition.Message(MSG_ERROR, "Array index out of bounds");
			delete this;
			return nullptr;
		}

		if (!arrayispointer)
		{
			// if this is an array within a class or another struct we can simplify the expression by creating a new PField with a cumulative offset.
			if (Array->ExprType == EFX_ClassMember || Array->ExprType == EFX_StructMember)
			{
				auto parentfield = static_cast<FxStructMember *>(Array)->membervar;
				// PFields are garbage collected so this will be automatically taken care of later.
				auto newfield = new PField(NAME_None, arraytype->ElementType, parentfield->Flags, indexval * arraytype->ElementSize + parentfield->Offset);
				static_cast<FxStructMember *>(Array)->membervar = newfield;
				Array->isresolved = false;	// re-resolve the parent so it can also check if it can be optimized away.
				auto x = Array->Resolve(ctx);
				Array = nullptr;
				return x;
			}
			else if (Array->ExprType == EFX_GlobalVariable)
			{
				auto parentfield = static_cast<FxGlobalVariable *>(Array)->membervar;
				auto newfield = new PField(NAME_None, arraytype->ElementType, parentfield->Flags, indexval * arraytype->ElementSize + parentfield->Offset);
				static_cast<FxGlobalVariable *>(Array)->membervar = newfield;
				Array->isresolved = false;	// re-resolve the parent so it can also check if it can be optimized away.
				auto x = Array->Resolve(ctx);
				Array = nullptr;
				return x;
			}
			else if (Array->ExprType == EFX_StackVariable)
			{
				auto parentfield = static_cast<FxStackVariable *>(Array)->membervar;
				auto newfield = new PField(NAME_None, arraytype->ElementType, parentfield->Flags, indexval * arraytype->ElementSize + parentfield->Offset);
				static_cast<FxStackVariable *>(Array)->ReplaceField(newfield);
				Array->isresolved = false;	// re-resolve the parent so it can also check if it can be optimized away.
				auto x = Array->Resolve(ctx);
				Array = nullptr;
				return x;
			}
		}
	}

	ValueType = arraytype->ElementType;
	if (!Array->RequestAddress(ctx, &AddressWritable))
	{
		ScriptPosition.Message(MSG_ERROR, "Unable to dereference array.");
		delete this;
		return nullptr;
	}
	return this;
}

//==========================================================================
//
// in its current state this won't be able to do more than handle the args array.
//
//==========================================================================

ExpEmit FxArrayElement::Emit(VMFunctionBuilder *build)
{
	PArray *arraytype;
	
	if (arrayispointer)
	{
		arraytype = static_cast<PArray*>(static_cast<PPointer*>(Array->ValueType)->PointedType);
	}
	else
	{
		arraytype = static_cast<PArray*>(Array->ValueType);
	}
	ExpEmit start = Array->Emit(build);

	/* what was this for?
	if (start.Konst)
	{
		ExpEmit tmpstart(build, REGT_POINTER);
		build->Emit(OP_LKP, tmpstart.RegNum, start.RegNum);
		start.Free(build);
		start = tmpstart;
	}
	*/
	if (index->isConstant())
	{
		unsigned indexval = static_cast<FxConstant *>(index)->GetValue().GetInt();
		assert(indexval < arraytype->ElementCount && "Array index out of bounds");

		if (AddressRequested)
		{
			if (indexval != 0)
			{
				indexval *= arraytype->ElementSize;
				if (!start.Fixed)
				{
					build->Emit(OP_ADDA_RK, start.RegNum, start.RegNum, build->GetConstantInt(indexval));
				}
				else
				{
					// do not clobber local variables.
					ExpEmit temp(build, start.RegType);
					build->Emit(OP_ADDA_RK, temp.RegNum, start.RegNum, build->GetConstantInt(indexval));
					start.Free(build);
					start = temp;
				}
			}
			return start;
		}
		else if (!start.Konst)
		{
			start.Free(build);
			ExpEmit dest(build, ValueType->GetRegType());
			build->Emit(arraytype->ElementType->GetLoadOp(), dest.RegNum, start.RegNum, build->GetConstantInt(indexval* arraytype->ElementSize));
			return dest;
		}
		else
		{
			static int LK_Ops[] = { OP_LK, OP_LKF, OP_LKS, OP_LKP };
			assert(start.RegType == ValueType->GetRegType());
			ExpEmit dest(build, start.RegType);
			build->Emit(LK_Ops[start.RegType], dest.RegNum, start.RegNum + indexval);
			return dest;
		}
	}
	else
	{
		ExpEmit indexv(index->Emit(build));
		// Todo: For dynamically allocated arrays (like global sector and linedef tables) we need to get the bound value in here somehow.
		// Right now their bounds are not properly checked for.
		if (arraytype->ElementCount > 65535)
		{
			build->Emit(OP_BOUND_K, indexv.RegNum, build->GetConstantInt(arraytype->ElementCount));
		}
		else
		{
			build->Emit(OP_BOUND, indexv.RegNum, arraytype->ElementCount);
		}

		if (!start.Konst)
		{
			int shiftbits = 0;
			while (1u << shiftbits < arraytype->ElementSize)
			{
				shiftbits++;
			}
			ExpEmit indexwork = indexv.Fixed && arraytype->ElementSize > 1 ? ExpEmit(build, indexv.RegType) : indexv;
			if (1u << shiftbits == arraytype->ElementSize)
			{
				if (shiftbits > 0)
				{
					build->Emit(OP_SLL_RI, indexwork.RegNum, indexv.RegNum, shiftbits);
				}
			}
			else
			{
				// A shift won't do, so use a multiplication
				build->Emit(OP_MUL_RK, indexwork.RegNum, indexv.RegNum, build->GetConstantInt(arraytype->ElementSize));
			}
			indexwork.Free(build);

			if (AddressRequested)
			{
				if (!start.Fixed)
				{
					build->Emit(OP_ADDA_RR, start.RegNum, start.RegNum, indexwork.RegNum);
				}
				else
				{
					start.Free(build);
					// do not clobber local variables.
					ExpEmit temp(build, start.RegType);
					build->Emit(OP_ADDA_RR, temp.RegNum, start.RegNum, indexwork.RegNum);
					start = temp;
				}
				return start;
			}
			else
			{
				start.Free(build);
				ExpEmit dest(build, ValueType->GetRegType());
				// added 1 to use the *_R version that takes the offset from a register
				build->Emit(arraytype->ElementType->GetLoadOp() + 1, dest.RegNum, start.RegNum, indexwork.RegNum);
				return dest;
			}
		}
		else
		{
			static int LKR_Ops[] = { OP_LK_R, OP_LKF_R, OP_LKS_R, OP_LKP_R };
			assert(start.RegType == ValueType->GetRegType());
			ExpEmit dest(build, start.RegType);
			if (start.RegNum <= 255)
			{
				// Since large constant tables are the exception, the constant component in C is an immediate value here.
				build->Emit(LKR_Ops[start.RegType], dest.RegNum, indexv.RegNum, start.RegNum);
			}
			else
			{
				build->Emit(OP_ADD_RK, indexv.RegNum, indexv.RegNum, build->GetConstantInt(start.RegNum));
				build->Emit(LKR_Ops[start.RegType], dest.RegNum, indexv.RegNum, 0);
			}
			indexv.Free(build);
			return dest;
		}
	}
}

//==========================================================================
//
// Checks if a function may be called from the current context.
//
//==========================================================================

static bool CheckFunctionCompatiblity(FScriptPosition &ScriptPosition, PFunction *caller, PFunction *callee)
{
	if (callee->Variants[0].Flags & VARF_Method)
	{
		// The called function must support all usage modes of the current function. It may support more, but must not support less.
		if ((callee->Variants[0].UseFlags & caller->Variants[0].UseFlags) != caller->Variants[0].UseFlags)
		{
			ScriptPosition.Message(MSG_ERROR, "Function %s incompatible with current context\n", callee->SymbolName.GetChars());
			return false;
		}

		if (!(caller->Variants[0].Flags & VARF_Method))
		{
			ScriptPosition.Message(MSG_ERROR, "Call to non-static function %s from a static context", callee->SymbolName.GetChars());
			return false;
		}
		else
		{
			auto callingself = caller->Variants[0].SelfClass;
			auto calledself = callee->Variants[0].SelfClass;
			bool match = (callingself == calledself);
			if (!match)
			{
				auto callingselfcls = dyn_cast<PClass>(caller->Variants[0].SelfClass);
				auto calledselfcls = dyn_cast<PClass>(callee->Variants[0].SelfClass);
				match = callingselfcls != nullptr && calledselfcls != nullptr && callingselfcls->IsDescendantOf(calledselfcls);
			}

			if (!match)
			{
				ScriptPosition.Message(MSG_ERROR, "Call to member function %s with incompatible self pointer.", callee->SymbolName.GetChars());
				return false;
			}
		}
	}
	return true;
}

//==========================================================================
//
//
//
//==========================================================================

FxFunctionCall::FxFunctionCall(FName methodname, FName rngname, FArgumentList &args, const FScriptPosition &pos)
: FxExpression(EFX_FunctionCall, pos)
{
	MethodName = methodname;
	RNG = &pr_exrandom;
	ArgList = std::move(args);
	if (rngname != NAME_None)
	{
		switch (MethodName)
		{
		case NAME_Random:
		case NAME_FRandom:
		case NAME_RandomPick:
		case NAME_FRandomPick:
		case NAME_Random2:
			RNG = FRandom::StaticFindRNG(rngname.GetChars());
			break;

		default:
			pos.Message(MSG_ERROR, "Cannot use named RNGs with %s", MethodName.GetChars());
			break;

		}
	}
}

//==========================================================================
//
//
//
//==========================================================================

FxFunctionCall::~FxFunctionCall()
{
}

//==========================================================================
//
// Check function that gets called
//
//==========================================================================

static bool CheckArgSize(FName fname, FArgumentList &args, int min, int max, FScriptPosition &sc)
{
	int s = args.Size();
	if (s < min)
	{
		sc.Message(MSG_ERROR, "Insufficient arguments in call to %s, expected %d, got %d", fname.GetChars(), min, s);
		return false;
	}
	else if (s > max && max >= 0)
	{
		sc.Message(MSG_ERROR, "Too many arguments in call to %s, expected %d, got %d", fname.GetChars(), min, s);
		return false;
	}
	return true;
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxFunctionCall::Resolve(FCompileContext& ctx)
{
	ABORT(ctx.Class);
	bool error = false;

	for (auto a : ArgList)
	{
		if (a == nullptr)
		{
			ScriptPosition.Message(MSG_ERROR, "Empty function argument.");
			delete this;
			return nullptr;
		}
	}

	PFunction *afd = FindClassMemberFunction(ctx.Class, ctx.Class, MethodName, ScriptPosition, &error);

	if (afd != nullptr)
	{
		if (!CheckFunctionCompatiblity(ScriptPosition, ctx.Function, afd))
		{
			delete this;
			return nullptr;
		}

		auto self = (afd->Variants[0].Flags & VARF_Method)? new FxSelf(ScriptPosition) : nullptr;
		auto x = new FxVMFunctionCall(self, afd, ArgList, ScriptPosition, false);
		delete this;
		return x->Resolve(ctx);
	}

	for (size_t i = 0; i < countof(FxFlops); ++i)
	{
		if (MethodName == FxFlops[i].Name)
		{
			FxExpression *x = new FxFlopFunctionCall(i, ArgList, ScriptPosition);
			delete this;
			return x->Resolve(ctx);
		}
	}

	int min, max, special;
	if (MethodName == NAME_ACS_NamedExecuteWithResult || MethodName == NAME_CallACS)
	{
		special = -ACS_ExecuteWithResult;
		min = 1;
		max = 5;
	}
	else
	{
		special = P_FindLineSpecial(MethodName.GetChars(), &min, &max);
	}
	if (special != 0 && min >= 0)
	{
		int paramcount = ArgList.Size();
		if (paramcount < min)
		{
			ScriptPosition.Message(MSG_ERROR, "Not enough parameters for '%s' (expected %d, got %d)", 
				MethodName.GetChars(), min, paramcount);
			delete this;
			return nullptr;
		}
		else if (paramcount > max)
		{
			ScriptPosition.Message(MSG_ERROR, "too many parameters for '%s' (expected %d, got %d)", 
				MethodName.GetChars(), max, paramcount);
			delete this;
			return nullptr;
		}
		FxExpression *self = (ctx.Function && ctx.Function->Variants[0].Flags & VARF_Method) ? new FxSelf(ScriptPosition) : nullptr;
		FxExpression *x = new FxActionSpecialCall(self, special, ArgList, ScriptPosition);
		delete this;
		return x->Resolve(ctx);
	}

	PClass *cls = PClass::FindClass(MethodName);
	if (cls != nullptr && cls->bExported)
	{
		if (CheckArgSize(MethodName, ArgList, 1, 1, ScriptPosition))
		{
			FxExpression *x = new FxDynamicCast(cls, ArgList[0]);
			ArgList[0] = nullptr;
			delete this;
			return x->Resolve(ctx);
		}
		else
		{
			delete this;
			return nullptr;
		}
	}


	// Last but not least: Check builtins and type casts. The random functions can take a named RNG if specified.
	// Note that for all builtins the used arguments have to be nulled in the ArgList so that they won't get deleted before they get used.
	FxExpression *func = nullptr;

	switch (MethodName)
	{
	case NAME_Color:
		if (ArgList.Size() == 3 || ArgList.Size() == 4)
		{
			func = new FxColorLiteral(ArgList, ScriptPosition);
			break;
		}
		// fall through
	case NAME_Bool:
	case NAME_Int:
	case NAME_uInt:
	case NAME_Float:
	case NAME_Double:
	case NAME_Name:
	case NAME_Sound:
	case NAME_State:
	case NAME_SpriteID:
	case NAME_TextureID:
		if (CheckArgSize(MethodName, ArgList, 1, 1, ScriptPosition))
		{
			PType *type = 
				MethodName == NAME_Bool ? TypeBool :
				MethodName == NAME_Int ? TypeSInt32 :
				MethodName == NAME_uInt ? TypeUInt32 :
				MethodName == NAME_Float ? TypeFloat64 :
				MethodName == NAME_Double ? TypeFloat64 :
				MethodName == NAME_Name ? TypeName :
				MethodName == NAME_SpriteID ? TypeSpriteID :
				MethodName == NAME_TextureID ? TypeTextureID :
				MethodName == NAME_State ? TypeState :
				MethodName == NAME_Color ? TypeColor : (PType*)TypeSound;

			func = new FxTypeCast(ArgList[0], type, true, true);
			ArgList[0] = nullptr;
		}
		break;

	case NAME_GetClass:
		if (CheckArgSize(NAME_GetClass, ArgList, 0, 0, ScriptPosition))
		{
			func = new FxGetClass(new FxSelf(ScriptPosition));
		}
		break;

	case NAME_GetDefaultByType:
		if (CheckArgSize(NAME_GetDefaultByType, ArgList, 1, 1, ScriptPosition))
		{
			func = new FxGetDefaultByType(ArgList[0]);
			ArgList[0] = nullptr;
		}
		break;

	case NAME_Random:
		// allow calling Random without arguments to default to (0, 255)
		if (ArgList.Size() == 0)
		{
			func = new FxRandom(RNG, new FxConstant(0, ScriptPosition), new FxConstant(255, ScriptPosition), ScriptPosition, ctx.FromDecorate);
		}
		else if (CheckArgSize(NAME_Random, ArgList, 2, 2, ScriptPosition))
		{
			func = new FxRandom(RNG, ArgList[0], ArgList[1], ScriptPosition, ctx.FromDecorate);
			ArgList[0] = ArgList[1] = nullptr;
		}
		break;

	case NAME_FRandom:
		if (CheckArgSize(NAME_FRandom, ArgList, 2, 2, ScriptPosition))
		{
			func = new FxFRandom(RNG, ArgList[0], ArgList[1], ScriptPosition);
			ArgList[0] = ArgList[1] = nullptr;
		}
		break;

	case NAME_RandomPick:
	case NAME_FRandomPick:
		if (CheckArgSize(MethodName, ArgList, 1, -1, ScriptPosition))
		{
			func = new FxRandomPick(RNG, ArgList, MethodName == NAME_FRandomPick, ScriptPosition, ctx.FromDecorate);
		}
		break;

	case NAME_Random2:
		if (CheckArgSize(NAME_Random2, ArgList, 0, 1, ScriptPosition))
		{
			func = new FxRandom2(RNG, ArgList.Size() == 0? nullptr : ArgList[0], ScriptPosition, ctx.FromDecorate);
			if (ArgList.Size() > 0) ArgList[0] = nullptr;
		}
		break;

	case NAME_Min:
	case NAME_Max:
		if (CheckArgSize(MethodName, ArgList, 2, -1, ScriptPosition))
		{
			func = new FxMinMax(ArgList, MethodName, ScriptPosition);
		}
		break;

	case NAME_Clamp:
		if (CheckArgSize(MethodName, ArgList, 3, 3, ScriptPosition))
		{
			TArray<FxExpression *> pass;
			pass.Resize(2);
			pass[0] = ArgList[0];
			pass[1] = ArgList[1];
			pass[0] = new FxMinMax(pass, NAME_Max, ScriptPosition);
			pass[1] = ArgList[2];
			func = new FxMinMax(pass, NAME_Min, ScriptPosition);
			ArgList[0] = ArgList[1] = ArgList[2] = nullptr;
		}
		break;

	case NAME_Abs:
		if (CheckArgSize(MethodName, ArgList, 1, 1, ScriptPosition))
		{
			func = new FxAbs(ArgList[0]);
			ArgList[0] = nullptr;
		}
		break;

	case NAME_ATan2:
	case NAME_VectorAngle:
		if (CheckArgSize(MethodName, ArgList, 2, 2, ScriptPosition))
		{
			func = MethodName == NAME_ATan2 ? new FxATan2(ArgList[0], ArgList[1], ScriptPosition) : new FxATan2(ArgList[1], ArgList[0], ScriptPosition);
			ArgList[0] = ArgList[1] = nullptr;
		}
		break;

	default:
		ScriptPosition.Message(MSG_ERROR, "Call to unknown function '%s'", MethodName.GetChars());
		break;
	}
	if (func != nullptr)
	{
		delete this;
		return func->Resolve(ctx);
	}
	delete this;
	return nullptr;
}


//==========================================================================
//
//
//
//==========================================================================

FxMemberFunctionCall::FxMemberFunctionCall(FxExpression *self, FName methodname, FArgumentList &args, const FScriptPosition &pos)
	: FxExpression(EFX_MemberFunctionCall, pos)
{
	Self = self;
	MethodName = methodname;
	ArgList = std::move(args);
}

//==========================================================================
//
//
//
//==========================================================================

FxMemberFunctionCall::~FxMemberFunctionCall()
{
	SAFE_DELETE(Self);
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxMemberFunctionCall::Resolve(FCompileContext& ctx)
{
	ABORT(ctx.Class);
	PStruct *cls;
	bool staticonly = false;
	bool novirtual = false;

	PStruct *ccls = nullptr;

	for (auto a : ArgList)
	{
		if (a == nullptr)
		{
			ScriptPosition.Message(MSG_ERROR, "Empty function argument.");
			delete this;
			return nullptr;
		}
	}

	if (Self->ExprType == EFX_Identifier)
	{
		// If the left side is a class name for a static member function call it needs to be resolved manually 
		// because the resulting value type would cause problems in nearly every other place where identifiers are being used.
		ccls = FindStructType(static_cast<FxIdentifier *>(Self)->Identifier);
		if (ccls != nullptr) static_cast<FxIdentifier *>(Self)->noglobal = true;
	}

	SAFE_RESOLVE(Self, ctx);

	if (Self->ValueType == TypeError)
	{
		if (ccls != nullptr)
		{
			if (!ccls->IsKindOf(RUNTIME_CLASS(PClass)) || static_cast<PClass *>(ccls)->bExported)
			{
				cls = ccls;
				staticonly = true;
				goto isresolved;
			}
		}
	}

	if (Self->ExprType == EFX_Super)
	{
		auto clstype = dyn_cast<PClass>(ctx.Function->Variants[0].SelfClass);
		if (clstype != nullptr)
		{
			// give the node the proper value type now that we know it's properly used.
			cls = clstype->ParentClass;
			Self->ValueType = NewPointer(cls);
			Self->ExprType = EFX_Self;
			novirtual = true;	// super calls are always non-virtual
		}
		else
		{
			ScriptPosition.Message(MSG_ERROR, "Super requires a class type");
		}
	}

	// Note: These builtins would better be relegated to the actual type objects, instead of polluting this file, but that's a task for later.

	// Texture builtins.
	if (Self->ValueType == TypeTextureID)
	{
		if (MethodName == NAME_IsValid || MethodName == NAME_IsNull || MethodName == NAME_Exists || MethodName == NAME_SetInvalid || MethodName == NAME_SetNull)
		{
			if (ArgList.Size() > 0)
			{
				ScriptPosition.Message(MSG_ERROR, "too many parameters in call to %s", MethodName.GetChars());
				delete this;
				return nullptr;
			}
			// No need to create a dedicated node here, all builtins map directly to trivial operations.
			Self->ValueType = TypeSInt32;	// all builtins treat the texture index as integer.
			FxExpression *x;
			switch (MethodName)
			{
			case NAME_IsValid:
				x = new FxCompareRel('>', Self, new FxConstant(0, ScriptPosition));
				break;

			case NAME_IsNull:
				x = new FxCompareEq(TK_Eq, Self, new FxConstant(0, ScriptPosition));
				break;

			case NAME_Exists:
				x = new FxCompareRel(TK_Geq, Self, new FxConstant(0, ScriptPosition));
				break;

			case NAME_SetInvalid:
				x = new FxAssign(Self, new FxConstant(-1, ScriptPosition));
				break;

			case NAME_SetNull:
				x = new FxAssign(Self, new FxConstant(0, ScriptPosition));
				break;
			}
			Self = nullptr;
			SAFE_RESOLVE(x, ctx);
			if (MethodName == NAME_SetInvalid || MethodName == NAME_SetNull) x->ValueType = TypeVoid; // override the default type of the assignment operator.
			delete this;
			return x;
		}
	}

	if (Self->IsVector())
	{
		// handle builtins: Vectors got 2: Length and Unit.
		if (MethodName == NAME_Length || MethodName == NAME_Unit)
		{
			if (ArgList.Size() > 0)
			{
				ScriptPosition.Message(MSG_ERROR, "too many parameters in call to %s", MethodName.GetChars());
				delete this;
				return nullptr;
			}
			auto x = new FxVectorBuiltin(Self, MethodName);
			Self = nullptr;
			delete this;
			return x->Resolve(ctx);
		}
	}

	if (Self->ValueType == TypeString)
	{
		// same for String methods. It also uses a hidden struct type to define them.
		Self->ValueType = TypeStringStruct;
	}

	if (Self->ValueType->IsKindOf(RUNTIME_CLASS(PPointer)))
	{
		auto ptype = static_cast<PPointer *>(Self->ValueType)->PointedType;
		if (ptype->IsKindOf(RUNTIME_CLASS(PStruct)))
		{
			if (ptype->IsKindOf(RUNTIME_CLASS(PClass)) && MethodName == NAME_GetClass)
			{
				if (ArgList.Size() > 0)
				{
					ScriptPosition.Message(MSG_ERROR, "too many parameters in call to %s", MethodName.GetChars());
					delete this;
					return nullptr;
				}
				auto x = new FxGetClass(Self);
				return x->Resolve(ctx);
			}
			cls = static_cast<PStruct *>(ptype);
		}
		else
		{
			ScriptPosition.Message(MSG_ERROR, "Left hand side of %s must point to a class object\n", MethodName.GetChars());
			delete this;
			return nullptr;
		}
	}
	else if (Self->ValueType->IsKindOf(RUNTIME_CLASS(PStruct)))
	{
		bool writable;
		if (Self->RequestAddress(ctx, &writable) && writable)
		{
			cls = static_cast<PStruct*>(Self->ValueType);
			Self->ValueType = NewPointer(Self->ValueType);
		}
		else
		{
			// Cannot be made writable so we cannot use its methods.
			ScriptPosition.Message(MSG_ERROR, "Invalid expression on left hand side of %s\n", MethodName.GetChars());
			delete this;
			return nullptr;
		}
	}
	else
	{
		ScriptPosition.Message(MSG_ERROR, "Invalid expression on left hand side of %s\n", MethodName.GetChars());
		delete this;
		return nullptr;
	}

	// Todo: handle member calls from instantiated structs.

isresolved:
	bool error = false;
	PFunction *afd = FindClassMemberFunction(cls, ctx.Class, MethodName, ScriptPosition, &error);
	if (error)
	{
		delete this;
		return nullptr;
	}

	if (afd == nullptr)
	{
		ScriptPosition.Message(MSG_ERROR, "Unknown function %s\n", MethodName.GetChars());
		delete this;
		return nullptr;
	}

	if (staticonly && (afd->Variants[0].Flags & VARF_Method))
	{
		auto clstype = dyn_cast<PClass>(ctx.Class);
		auto ccls = dyn_cast<PClass>(cls);
		if (clstype == nullptr || ccls == nullptr || !clstype->IsDescendantOf(ccls))
		{
			ScriptPosition.Message(MSG_ERROR, "Cannot call non-static function %s::%s from here\n", cls->TypeName.GetChars(), MethodName.GetChars());
			delete this;
			return nullptr;
		}
		else
		{
			// Todo: If this is a qualified call to a parent class function, let it through (but this needs to disable virtual calls later.)
			ScriptPosition.Message(MSG_ERROR, "Qualified member call to parent class not yet implemented\n", cls->TypeName.GetChars(), MethodName.GetChars());
			delete this;
			return nullptr;
		}
	}

	if (afd->Variants[0].Flags & VARF_Method)
	{
		if (Self->ExprType == EFX_Self)
		{
			if (!CheckFunctionCompatiblity(ScriptPosition, ctx.Function, afd))
			{
				delete this;
				return nullptr;
			}
		}
		else
		{
			// Functions with no Actor usage may not be called through a pointer because they will lose their context.
			if (!(afd->Variants[0].UseFlags & SUF_ACTOR))
			{
				ScriptPosition.Message(MSG_ERROR, "Function %s cannot be used with a non-self object\n", afd->SymbolName.GetChars());
				delete this;
				return nullptr;
			}
		}
	}

	// do not pass the self pointer to static functions.
	auto self = (afd->Variants[0].Flags & VARF_Method) ? Self : nullptr;
	auto x = new FxVMFunctionCall(self, afd, ArgList, ScriptPosition, staticonly|novirtual);
	if (Self == self) Self = nullptr;
	delete this;
	return x->Resolve(ctx);
}


//==========================================================================
//
// FxActionSpecialCall
//
// If special is negative, then the first argument will be treated as a
// name for ACS_NamedExecuteWithResult.
//
//==========================================================================

FxActionSpecialCall::FxActionSpecialCall(FxExpression *self, int special, FArgumentList &args, const FScriptPosition &pos)
: FxExpression(EFX_ActionSpecialCall, pos)
{
	Self = self;
	Special = special;
	ArgList = std::move(args);
	EmitTail = false;
}

//==========================================================================
//
//
//
//==========================================================================

FxActionSpecialCall::~FxActionSpecialCall()
{
	SAFE_DELETE(Self);
}

//==========================================================================
//
//
//
//==========================================================================

PPrototype *FxActionSpecialCall::ReturnProto()
{
	EmitTail = true;
	return FxExpression::ReturnProto();
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxActionSpecialCall::Resolve(FCompileContext& ctx)
{
	CHECKRESOLVED();
	bool failed = false;

	SAFE_RESOLVE_OPT(Self, ctx);
	for (unsigned i = 0; i < ArgList.Size(); i++)
	{
		ArgList[i] = ArgList[i]->Resolve(ctx);
		if (ArgList[i] == nullptr)
		{
			failed = true;
		}
		else if (Special < 0 && i == 0)
		{
			if (ArgList[i]->ValueType == TypeString)
			{
				ArgList[i] = new FxNameCast(ArgList[i]);
				ArgList[i] = ArgList[i]->Resolve(ctx);
				if (ArgList[i] == nullptr)
				{
					failed = true;
				}
			}
			else if (ArgList[i]->ValueType != TypeName)
			{
				ScriptPosition.Message(MSG_ERROR, "Name expected for parameter %d", i);
				failed = true;
			}
		}
		else if (!ArgList[i]->IsInteger())
		{
			if (ArgList[i]->ValueType->GetRegType() == REGT_FLOAT /* lax */)
			{
				ArgList[i] = new FxIntCast(ArgList[i], ctx.FromDecorate);
			}
			else
			{
				ScriptPosition.Message(MSG_ERROR, "Integer expected for parameter %d", i);
				failed = true;
			}
		}
	}
	if (failed)
	{
		delete this;
		return nullptr;
	}
	ValueType = TypeSInt32;
	return this;
}


//==========================================================================
//
// 
//
//==========================================================================

int BuiltinCallLineSpecial(VMValue *param, TArray<VMValue> &defaultparam, int numparam, VMReturn *ret, int numret)
{
	assert(numparam > 2 && numparam < 8);
	assert(param[0].Type == REGT_INT);
	assert(param[1].Type == REGT_POINTER);
	int v[5] = { 0 };

	for (int i = 2; i < numparam; ++i)
	{
		v[i - 2] = param[i].i;
	}
	ACTION_RETURN_INT(P_ExecuteSpecial(param[0].i, nullptr, reinterpret_cast<AActor*>(param[1].a), false, v[0], v[1], v[2], v[3], v[4]));
}

ExpEmit FxActionSpecialCall::Emit(VMFunctionBuilder *build)
{
	unsigned i = 0;

	build->Emit(OP_PARAMI, abs(Special));			// pass special number
	// fixme: This really should use the Self pointer that got passed to this class instead of just using the first argument from the function. 
	// Once static functions are possible, or specials can be called through a member access operator this won't work anymore.
	build->Emit(OP_PARAM, 0, REGT_POINTER, 0);		// pass self 
	for (; i < ArgList.Size(); ++i)
	{
		FxExpression *argex = ArgList[i];
		if (Special < 0 && i == 0)
		{
			assert(argex->ValueType == TypeName);
			assert(argex->isConstant());
			build->EmitParamInt(-static_cast<FxConstant *>(argex)->GetValue().GetName());
		}
		else
		{
			assert(argex->ValueType->GetRegType() == REGT_INT);
			if (argex->isConstant())
			{
				build->EmitParamInt(static_cast<FxConstant *>(argex)->GetValue().GetInt());
			}
			else
			{
				ExpEmit arg(argex->Emit(build));
				build->Emit(OP_PARAM, 0, arg.RegType, arg.RegNum);
				arg.Free(build);
			}
		}
	}
	// Call the BuiltinCallLineSpecial function to perform the desired special.
	VMFunction *callfunc;
	PSymbol *sym = FindBuiltinFunction(NAME_BuiltinCallLineSpecial, BuiltinCallLineSpecial);

	assert(sym->IsKindOf(RUNTIME_CLASS(PSymbolVMFunction)));
	assert(((PSymbolVMFunction *)sym)->Function != nullptr);
	callfunc = ((PSymbolVMFunction *)sym)->Function;
	ArgList.DeleteAndClear();
	ArgList.ShrinkToFit();

	if (EmitTail)
	{
		build->Emit(OP_TAIL_K, build->GetConstantAddress(callfunc, ATAG_OBJECT), 2 + i, 0);
		ExpEmit call;
		call.Final = true;
		return call;
	}

	ExpEmit dest(build, REGT_INT);
	build->Emit(OP_CALL_K, build->GetConstantAddress(callfunc, ATAG_OBJECT), 2 + i, 1);
	build->Emit(OP_RESULT, 0, REGT_INT, dest.RegNum);
	return dest;
}

//==========================================================================
//
// FxVMFunctionCall
//
//==========================================================================

FxVMFunctionCall::FxVMFunctionCall(FxExpression *self, PFunction *func, FArgumentList &args, const FScriptPosition &pos, bool novirtual)
: FxExpression(EFX_VMFunctionCall, pos)
{
	Self = self;
	Function = func;
	ArgList = std::move(args);
	EmitTail = false;
	NoVirtual = novirtual;
}

//==========================================================================
//
//
//
//==========================================================================

FxVMFunctionCall::~FxVMFunctionCall()
{
}

//==========================================================================
//
//
//
//==========================================================================

PPrototype *FxVMFunctionCall::ReturnProto()
{
	EmitTail = true;
	return Function->Variants[0].Proto;
}

//==========================================================================
//
//
//
//==========================================================================

VMFunction *FxVMFunctionCall::GetDirectFunction()
{
	// If this return statement calls a non-virtual function with no arguments,
	// then it can be a "direct" function. That is, the DECORATE
	// definition can call that function directly without wrapping
	// it inside VM code.
	if (ArgList.Size() == 0 && !(Function->Variants[0].Flags & VARF_Virtual))
	{
		unsigned imp = Function->GetImplicitArgs();
		if (Function->Variants[0].ArgFlags.Size() > imp && !(Function->Variants[0].ArgFlags[imp] & VARF_Optional)) return nullptr;
		return Function->Variants[0].Implementation;
	}
	
	return nullptr;
}

//==========================================================================
//
// FxVMFunctionCall :: Resolve
//
//==========================================================================

FxExpression *FxVMFunctionCall::Resolve(FCompileContext& ctx)
{
	CHECKRESOLVED();
	SAFE_RESOLVE_OPT(Self, ctx);
	bool failed = false;
	auto proto = Function->Variants[0].Proto;
	auto &argtypes = proto->ArgumentTypes;
	auto &argnames = Function->Variants[0].ArgNames;
	auto &argflags = Function->Variants[0].ArgFlags;
	auto &defaults = Function->Variants[0].Implementation->DefaultArgs;

	int implicit = Function->GetImplicitArgs();

	// This should never happen.
	if (Self == nullptr && (Function->Variants[0].Flags & VARF_Method))
	{
		ScriptPosition.Message(MSG_ERROR, "Call to non-static function without a self pointer");
		delete this;
		return nullptr;
	}

	if (ArgList.Size() > 0)
	{
		bool foundvarargs = false;
		PType * type = nullptr;
		int flag = 0;
		if (argtypes.Last() != nullptr && ArgList.Size() + implicit > argtypes.Size())
		{
			ScriptPosition.Message(MSG_ERROR, "Too many arguments in call to %s", Function->SymbolName.GetChars());
			delete this;
			return nullptr;
		}

		for (unsigned i = 0; i < ArgList.Size(); i++)
		{
			// Varargs must all have the same type as the last typed argument. A_Jump is the only function using it.
			if (!foundvarargs)
			{
				if (argtypes[i + implicit] == nullptr) foundvarargs = true;
				else
				{
					type = argtypes[i + implicit];
					flag = argflags[i + implicit];
				}
			}
			assert(type != nullptr);

			if (ArgList[i]->ExprType == EFX_NamedNode)
			{
				if (!(flag & VARF_Optional))
				{
					ScriptPosition.Message(MSG_ERROR, "Cannot use a named argument here - not all required arguments have been passed.");
					delete this;
					return nullptr;
				}
				if (foundvarargs)
				{
					ScriptPosition.Message(MSG_ERROR, "Cannot use a named argument in the varargs part of the parameter list.");
					delete this;
					return nullptr;
				}
				unsigned j;
				bool done = false;
				FName name = static_cast<FxNamedNode *>(ArgList[i])->name;
				for (j = 0; j < argnames.Size() - implicit; j++)
				{
					if (argnames[j + implicit] == name)
					{
						if (j < i)
						{
							ScriptPosition.Message(MSG_ERROR, "Named argument %s comes before current position in argument list.", name.GetChars());
							delete this;
							return nullptr;
						}
						// copy the original argument into the list
						auto old = static_cast<FxNamedNode *>(ArgList[i]);
						ArgList[i] = old->value; 
						old->value = nullptr;
						delete old;
						// now fill the gap with constants created from the default list so that we got a full list of arguments.
						int insert = j - i;
						for (int k = 0; k < insert; k++)
						{
							auto ntype = argtypes[i + k + implicit];
							// If this is a reference argument, the pointer type must be undone because the code below expects the pointed type as value type.
							if (argflags[i + k + implicit] & VARF_Ref)
							{
								assert(ntype->IsKindOf(RUNTIME_CLASS(PPointer)));
								ntype = TypeNullPtr; // the default of a reference type can only be a null pointer
							}
							auto x = new FxConstant(ntype, defaults[i + k + implicit], ScriptPosition);
							ArgList.Insert(i + k, x);
						}
						done = true;
						break;
					}
				}
				if (!done)
				{
					ScriptPosition.Message(MSG_ERROR, "Named argument %s not found.", name.GetChars());
					delete this;
					return nullptr;
				}
				// re-get the proper info for the inserted node.
				type = argtypes[i + implicit];
				flag = argflags[i + implicit];
			}

			FxExpression *x;
			if (!(flag & (VARF_Ref|VARF_Out)))
			{
				x = new FxTypeCast(ArgList[i], type, false);
				x = x->Resolve(ctx);
			}
			else
			{
				bool writable;
				ArgList[i] = ArgList[i]->Resolve(ctx);	// nust be resolved before the address is requested.
				if (ArgList[i] != nullptr && ArgList[i]->ValueType != TypeNullPtr)
				{
					ArgList[i]->RequestAddress(ctx, &writable);
					if (flag & VARF_Ref) ArgList[i]->ValueType = NewPointer(ArgList[i]->ValueType);
					// For a reference argument the types must match 100%.
					if (type != ArgList[i]->ValueType)
					{
						ScriptPosition.Message(MSG_ERROR, "Type mismatch in reference argument", Function->SymbolName.GetChars());
						x = nullptr;
					}
					else
					{
						x = ArgList[i];
					}
				}
				else x = ArgList[i];
			}
			failed |= (x == nullptr);
			ArgList[i] = x;
		}
		int numargs = ArgList.Size() + implicit;
		if ((unsigned)numargs < argtypes.Size() && argtypes[numargs] != nullptr)
		{
			auto flags = Function->Variants[0].ArgFlags[numargs];
			if (!(flags & VARF_Optional))
			{
				ScriptPosition.Message(MSG_ERROR, "Insufficient arguments in call to %s", Function->SymbolName.GetChars());
				delete this;
				return nullptr;
			}
		}
	}
	else
	{
		if ((unsigned)implicit < argtypes.Size() && argtypes[implicit] != nullptr)
		{
			auto flags = Function->Variants[0].ArgFlags[implicit];
			if (!(flags & VARF_Optional))
			{
				ScriptPosition.Message(MSG_ERROR, "Insufficient arguments in call to %s", Function->SymbolName.GetChars());
				delete this;
				return nullptr;
			}
		}
	}
	if (failed)
	{
		delete this;
		return nullptr;
	}
	TArray<PType *> &rets = proto->ReturnTypes;
	if (rets.Size() > 0)
	{
		ValueType = rets[0];
	}
	else
	{
		ValueType = TypeVoid;
	}
	return this;
}

//==========================================================================
//
//
//
//==========================================================================

ExpEmit FxVMFunctionCall::Emit(VMFunctionBuilder *build)
{
	assert(build->Registers[REGT_POINTER].GetMostUsed() >= build->NumImplicits);
	int count = 0;

	if (count == 1)
	{
		ExpEmit reg;
		if (CheckEmitCast(build, EmitTail, reg))
		{
			ArgList.DeleteAndClear();
			ArgList.ShrinkToFit();
			return reg;
		}
	}

	VMFunction *vmfunc = Function->Variants[0].Implementation;
	bool staticcall = (vmfunc->Final || vmfunc->VirtualIndex == ~0u || NoVirtual);

	count = 0;
	// Emit code to pass implied parameters
	ExpEmit selfemit;
	if (Function->Variants[0].Flags & VARF_Method)
	{
		assert(Self != nullptr);
		selfemit = Self->Emit(build);
		assert((selfemit.RegType == REGT_POINTER) || (selfemit.Fixed && selfemit.Target));
		if (selfemit.Fixed && selfemit.Target)
		{
			// Address of a local variable.
			build->Emit(OP_PARAM, 0, selfemit.RegType | REGT_ADDROF, selfemit.RegNum);
		}
		else
		{
			build->Emit(OP_PARAM, 0, selfemit.RegType, selfemit.RegNum);
		}
		count += 1;
		if (Function->Variants[0].Flags & VARF_Action)
		{
			static_assert(NAP == 3, "This code needs to be updated if NAP changes");
			if (build->NumImplicits == NAP && selfemit.RegNum == 0)	// only pass this function's stateowner and stateinfo if the subfunction is run in self's context.
			{
				build->Emit(OP_PARAM, 0, REGT_POINTER, 1);
				build->Emit(OP_PARAM, 0, REGT_POINTER, 2);
			}
			else
			{
				// pass self as stateowner, otherwise all attempts of the subfunction to retrieve a state from a name would fail.
				build->Emit(OP_PARAM, 0, selfemit.RegType, selfemit.RegNum);
				build->Emit(OP_PARAM, 0, REGT_POINTER | REGT_KONST, build->GetConstantAddress(nullptr, ATAG_GENERIC));
			}
			count += 2;
		}
		if (staticcall) selfemit.Free(build);
	}
	else staticcall = true;
	// Emit code to pass explicit parameters
	for (unsigned i = 0; i < ArgList.Size(); ++i)
	{
		count += EmitParameter(build, ArgList[i], ScriptPosition);
	}
	ArgList.DeleteAndClear();
	ArgList.ShrinkToFit();

	// Get a constant register for this function
	if (staticcall)
	{
		int funcaddr = build->GetConstantAddress(vmfunc, ATAG_OBJECT);
		// Emit the call
		if (EmitTail)
		{ // Tail call
			build->Emit(OP_TAIL_K, funcaddr, count, 0);
			ExpEmit call;
			call.Final = true;
			return call;
		}
		else if (vmfunc->Proto->ReturnTypes.Size() > 0)
		{ // Call, expecting one result
			build->Emit(OP_CALL_K, funcaddr, count, MAX(1, AssignCount));
			goto handlereturns;
		}
		else
		{ // Call, expecting no results
			build->Emit(OP_CALL_K, funcaddr, count, 0);
			return ExpEmit();
		}
	}
	else
	{
		selfemit.Free(build);
		ExpEmit funcreg(build, REGT_POINTER);
		build->Emit(OP_VTBL, funcreg.RegNum, selfemit.RegNum, vmfunc->VirtualIndex);
		if (EmitTail)
		{ // Tail call
			build->Emit(OP_TAIL, funcreg.RegNum, count, 0);
			ExpEmit call;
			call.Final = true;
			return call;
		}
		else if (vmfunc->Proto->ReturnTypes.Size() > 0)
		{ // Call, expecting one result
			build->Emit(OP_CALL, funcreg.RegNum, count, MAX(1, AssignCount));
			goto handlereturns;
		}
		else
		{ // Call, expecting no results
			build->Emit(OP_CALL, funcreg.RegNum, count, 0);
			return ExpEmit();
		}
	}
handlereturns:
	if (AssignCount == 0)
	{
		// Regular call, will not write to ReturnRegs
		ExpEmit reg(build, vmfunc->Proto->ReturnTypes[0]->GetRegType(), vmfunc->Proto->ReturnTypes[0]->GetRegCount());
		build->Emit(OP_RESULT, 0, EncodeRegType(reg), reg.RegNum);
		return reg;
	}
	else
	{
		// Multi-Assignment call, this must fill in the ReturnRegs array so that the multi-assignment operator can dispatch the return values.
		assert((unsigned)AssignCount <= vmfunc->Proto->ReturnTypes.Size());
		for (int i = 0; i < AssignCount; i++)
		{
			ExpEmit reg(build, vmfunc->Proto->ReturnTypes[i]->GetRegType(), vmfunc->Proto->ReturnTypes[i]->GetRegCount());
			build->Emit(OP_RESULT, 0, EncodeRegType(reg), reg.RegNum);
			ReturnRegs.Push(reg);
		}
		return ExpEmit();
	}
}

//==========================================================================
//
// If calling one of the casting kludge functions, don't bother calling the
// function; just use the parameter directly. Returns true if this was a
// kludge function, false otherwise.
//
//==========================================================================

bool FxVMFunctionCall::CheckEmitCast(VMFunctionBuilder *build, bool returnit, ExpEmit &reg)
{
	FName funcname = Function->SymbolName;
	if (funcname == NAME___decorate_internal_int__ ||
		funcname == NAME___decorate_internal_bool__ ||
		funcname == NAME___decorate_internal_float__)
	{
		FxExpression *arg = ArgList[0];
		if (returnit)
		{
			if (arg->isConstant() &&
				(funcname == NAME___decorate_internal_int__ ||
				 funcname == NAME___decorate_internal_bool__))
			{ // Use immediate version for integers in range
				build->EmitRetInt(0, true, static_cast<FxConstant *>(arg)->GetValue().Int);
			}
			else
			{
				ExpEmit where = arg->Emit(build);
				build->Emit(OP_RET, RET_FINAL, EncodeRegType(where), where.RegNum);
				where.Free(build);
			}
			reg = ExpEmit();
			reg.Final = true;
		}
		else
		{
			reg = arg->Emit(build);
		}
		return true;
	}
	return false;
}

//==========================================================================
//
//
//
//==========================================================================

FxFlopFunctionCall::FxFlopFunctionCall(size_t index, FArgumentList &args, const FScriptPosition &pos)
: FxExpression(EFX_FlopFunctionCall, pos)
{
	assert(index < countof(FxFlops) && "FLOP index out of range");
	Index = (int)index;
	ArgList = std::move(args);
}

//==========================================================================
//
//
//
//==========================================================================

FxFlopFunctionCall::~FxFlopFunctionCall()
{
}

FxExpression *FxFlopFunctionCall::Resolve(FCompileContext& ctx)
{
	CHECKRESOLVED();

	if (ArgList.Size() != 1)
	{
		ScriptPosition.Message(MSG_ERROR, "%s only has one parameter", FName(FxFlops[Index].Name).GetChars());
		delete this;
		return nullptr;
	}

	ArgList[0] = ArgList[0]->Resolve(ctx);
	if (ArgList[0] == nullptr)
	{
		delete this;
		return nullptr;
	}

	if (!ArgList[0]->IsNumeric())
	{
		ScriptPosition.Message(MSG_ERROR, "numeric value expected for parameter");
		delete this;
		return nullptr;
	}
	if (ArgList[0]->isConstant())
	{
		double v = static_cast<FxConstant *>(ArgList[0])->GetValue().GetFloat();
		v = FxFlops[Index].Evaluate(v);
		FxExpression *x = new FxConstant(v, ScriptPosition);
		delete this;
		return x;
	}
	if (ArgList[0]->ValueType->GetRegType() == REGT_INT)
	{
		ArgList[0] = new FxFloatCast(ArgList[0]);
	}
	ValueType = TypeFloat64;
	return this;
}

//==========================================================================
//
//
//==========================================================================

ExpEmit FxFlopFunctionCall::Emit(VMFunctionBuilder *build)
{
	assert(ValueType == ArgList[0]->ValueType);
	ExpEmit from = ArgList[0]->Emit(build);
	ExpEmit to;
	assert(from.Konst == 0);
	assert(ValueType->GetRegCount() == 1);
	// Do it in-place, unless a local variable
	if (from.Fixed)
	{
		to = ExpEmit(build, from.RegType);
		from.Free(build);
	}
	else
	{
		to = from;
	}

	build->Emit(OP_FLOP, to.RegNum, from.RegNum, FxFlops[Index].Flop);
	ArgList.DeleteAndClear();
	ArgList.ShrinkToFit();
	return to;
}


//==========================================================================
//
//
//==========================================================================

FxVectorBuiltin::FxVectorBuiltin(FxExpression *self, FName name)
	:FxExpression(EFX_VectorBuiltin, self->ScriptPosition)
{
	Self = self;
	Function = name;
}

FxVectorBuiltin::~FxVectorBuiltin()
{
	SAFE_DELETE(Self);
}

FxExpression *FxVectorBuiltin::Resolve(FCompileContext &ctx)
{
	SAFE_RESOLVE(Self, ctx);
	assert(Self->IsVector());	// should never be created for anything else.
	ValueType = Function == NAME_Length ? TypeFloat64 : Self->ValueType;
	return this;
}

ExpEmit FxVectorBuiltin::Emit(VMFunctionBuilder *build)
{
	ExpEmit to(build, ValueType->GetRegType(), ValueType->GetRegCount());
	ExpEmit op = Self->Emit(build);
	if (Function == NAME_Length)
	{
		build->Emit(Self->ValueType == TypeVector2 ? OP_LENV2 : OP_LENV3, to.RegNum, op.RegNum);
	}
	else
	{
		ExpEmit len(build, REGT_FLOAT);
		build->Emit(Self->ValueType == TypeVector2 ? OP_LENV2 : OP_LENV3, len.RegNum, op.RegNum);
		build->Emit(Self->ValueType == TypeVector2 ? OP_DIVVF2_RR : OP_DIVVF3_RR, to.RegNum, op.RegNum, len.RegNum);
		len.Free(build);
	}
	op.Free(build);
	return to;
}

//==========================================================================
//
//
//==========================================================================

FxGetClass::FxGetClass(FxExpression *self)
	:FxExpression(EFX_GetClass, self->ScriptPosition)
{
	Self = self;
}

FxGetClass::~FxGetClass()
{
	SAFE_DELETE(Self);
}

FxExpression *FxGetClass::Resolve(FCompileContext &ctx)
{
	SAFE_RESOLVE(Self, ctx);
	if (!Self->IsObject())
	{
		ScriptPosition.Message(MSG_ERROR, "GetClass() requires an object");
		delete this;
		return nullptr;
	}
	ValueType = NewClassPointer(static_cast<PClass*>(static_cast<PPointer*>(Self->ValueType)->PointedType));
	return this;
}

ExpEmit FxGetClass::Emit(VMFunctionBuilder *build)
{
	ExpEmit op = Self->Emit(build);
	op.Free(build);
	ExpEmit to(build, REGT_POINTER);
	build->Emit(OP_META, to.RegNum, op.RegNum);
	return to;
}

//==========================================================================
//
//
//==========================================================================

FxGetDefaultByType::FxGetDefaultByType(FxExpression *self)
	:FxExpression(EFX_GetDefaultByType, self->ScriptPosition)
{
	Self = self;
}

FxGetDefaultByType::~FxGetDefaultByType()
{
	SAFE_DELETE(Self);
}

FxExpression *FxGetDefaultByType::Resolve(FCompileContext &ctx)
{
	SAFE_RESOLVE(Self, ctx);
	PClass *cls = nullptr;

	if (Self->ValueType == TypeString || Self->ValueType == TypeName)
	{
		if (Self->isConstant())
		{
			cls = PClass::FindActor(static_cast<FxConstant *>(Self)->GetValue().GetName());
			if (cls == nullptr)
			{
				ScriptPosition.Message(MSG_ERROR, "GetDefaultByType() requires an actor class type, but got %s", static_cast<FxConstant *>(Self)->GetValue().GetString().GetChars());
				delete this;
				return nullptr;
			}
			Self = new FxConstant(cls, NewClassPointer(cls), ScriptPosition);
		}
		else
		{
			// this is the ugly case. We do not know what we have and cannot do proper type casting.
			// For now error out and let this case require explicit handling on the user side.
			ScriptPosition.Message(MSG_ERROR, "GetDefaultByType() requires an actor class type", static_cast<FxConstant *>(Self)->GetValue().GetString().GetChars());
			delete this;
			return nullptr;
		}
	}
	else
	{
		auto cp = dyn_cast<PClassPointer>(Self->ValueType);
		if (cp == nullptr || !cp->ClassRestriction->IsDescendantOf(RUNTIME_CLASS(AActor)))
		{
			ScriptPosition.Message(MSG_ERROR, "GetDefaultByType() requires an actor class type");
			delete this;
			return nullptr;
		}
		cls = cp->ClassRestriction;
	}
	ValueType = NewPointer(cls, true);
	return this;
}

ExpEmit FxGetDefaultByType::Emit(VMFunctionBuilder *build)
{
	ExpEmit op = Self->Emit(build);
	op.Free(build);
	ExpEmit to(build, REGT_POINTER);
	if (op.Konst)
	{
		build->Emit(OP_LKP, to.RegNum, op.RegNum);
		op = to;
	}
	build->Emit(OP_LO, to.RegNum, op.RegNum, build->GetConstantInt(myoffsetof(PClass, Defaults)));
	return to;
}

//==========================================================================
//
//
//==========================================================================

FxColorLiteral::FxColorLiteral(FArgumentList &args, FScriptPosition &sc)
	:FxExpression(EFX_ColorLiteral, sc)
{
	ArgList = std::move(args);
}

FxExpression *FxColorLiteral::Resolve(FCompileContext &ctx)
{
	CHECKRESOLVED();
	unsigned constelements = 0;
	assert(ArgList.Size() == 3 || ArgList.Size() == 4);
	if (ArgList.Size() == 3) ArgList.Insert(0, nullptr);
	for (int i = 0; i < 4; i++)
	{
		if (ArgList[i] != nullptr)
		{
			SAFE_RESOLVE(ArgList[i], ctx);
			if (!ArgList[i]->IsInteger())
			{
				ScriptPosition.Message(MSG_ERROR, "Integer expected for color component");
				delete this;
				return nullptr;
			}
			if (ArgList[i]->isConstant())
			{
				constval += clamp(static_cast<FxConstant *>(ArgList[i])->GetValue().GetInt(), 0, 255) << (24 - i * 8);
				delete ArgList[i];
				ArgList[i] = nullptr;
				constelements++;
			}
		}
		else constelements++;
	}
	if (constelements == 4)
	{
		auto x = new FxConstant(constval, ScriptPosition);
		x->ValueType = TypeColor;
		delete this;
		return x;
	}
	ValueType = TypeColor;
	return this;
}

ExpEmit FxColorLiteral::Emit(VMFunctionBuilder *build)
{
	ExpEmit out(build, REGT_INT);
	build->Emit(OP_LK, out.RegNum, build->GetConstantInt(constval));
	for (int i = 0; i < 4; i++)
	{
		if (ArgList[i] != nullptr)
		{
			assert(!ArgList[i]->isConstant());
			ExpEmit in = ArgList[i]->Emit(build);
			in.Free(build);
			ExpEmit work(build, REGT_INT);
			build->Emit(OP_MAX_RK, work.RegNum, in.RegNum, build->GetConstantInt(0));
			build->Emit(OP_MIN_RK, work.RegNum, work.RegNum, build->GetConstantInt(255));
			if (i != 3) build->Emit(OP_SLL_RI, work.RegNum, work.RegNum, 24 - (i * 8));
			build->Emit(OP_OR_RR, out.RegNum, out.RegNum, work.RegNum);
		}
	}
	return out;
}

//==========================================================================
//
// FxSequence :: Resolve
//
//==========================================================================

FxExpression *FxSequence::Resolve(FCompileContext &ctx)
{
	CHECKRESOLVED();
	bool fail = false;
	for (unsigned i = 0; i < Expressions.Size(); ++i)
	{
		if (nullptr == (Expressions[i] = Expressions[i]->Resolve(ctx)))
		{
			fail = true;
		}
		else if (Expressions[i]->ValueType == TypeError)
		{
			ScriptPosition.Message(MSG_ERROR, "Invalid statement");
			fail = true;
		}
	}
	if (fail)
	{
		delete this;
		return nullptr;
	}
	return this;
}

//==========================================================================
//
// FxSequence :: CheckReturn
//
//==========================================================================

bool FxSequence::CheckReturn()
{
	// a sequence always returns when its last element returns.
	return Expressions.Size() > 0 && Expressions.Last()->CheckReturn();
}

//==========================================================================
//
// FxSequence :: Emit
//
//==========================================================================

ExpEmit FxSequence::Emit(VMFunctionBuilder *build)
{
	for (unsigned i = 0; i < Expressions.Size(); ++i)
	{
		ExpEmit v = Expressions[i]->Emit(build);
		// Throw away any result. We don't care about it.
		v.Free(build);
	}
	return ExpEmit();
}

//==========================================================================
//
// FxSequence :: GetDirectFunction
//
//==========================================================================

VMFunction *FxSequence::GetDirectFunction()
{
	if (Expressions.Size() == 1)
	{
		return Expressions[0]->GetDirectFunction();
	}
	return nullptr;
}

//==========================================================================
//
// FxCompoundStatement :: Resolve
//
//==========================================================================

FxExpression *FxCompoundStatement::Resolve(FCompileContext &ctx)
{
	auto outer = ctx.Block;
	Outer = ctx.Block;
	ctx.Block = this;
	auto x = FxSequence::Resolve(ctx);
	ctx.Block = outer;
	return x;
}

//==========================================================================
//
// FxCompoundStatement :: Emit
//
//==========================================================================

ExpEmit FxCompoundStatement::Emit(VMFunctionBuilder *build)
{
	auto e = FxSequence::Emit(build);
	// Release all local variables in this block.
	for (auto l : LocalVars)
	{
		l->Release(build);
	}
	return e;
}

//==========================================================================
//
// FxCompoundStatement :: FindLocalVariable
//
// Looks for a variable name in any of the containing compound statements
// This does a simple linear search on each block's variables. 
// The lists here normally don't get large enough to justify something more complex.
//
//==========================================================================

FxLocalVariableDeclaration *FxCompoundStatement::FindLocalVariable(FName name, FCompileContext &ctx)
{
	auto block = this;
	while (block != nullptr)
	{
		for (auto l : block->LocalVars)
		{
			if (l->Name == name)
			{
				return l;
			}
		}
		block = block->Outer;
	}
	// finally check the context for function arguments
	for (auto arg : ctx.FunctionArgs)
	{
		if (arg->Name == name)
		{
			return arg;
		}
	}
	return nullptr;
}

//==========================================================================
//
// FxCompoundStatement :: CheckLocalVariable
//
// Checks if the current block already contains a local variable 
// of the given name.
//
//==========================================================================

bool FxCompoundStatement::CheckLocalVariable(FName name)
{
	for (auto l : LocalVars)
	{
		if (l->Name == name)
		{
			return true;
		}
	}
	return false;
}

//==========================================================================
//
// FxSwitchStatement
//
//==========================================================================

FxSwitchStatement::FxSwitchStatement(FxExpression *cond, FArgumentList &content, const FScriptPosition &pos)
	: FxExpression(EFX_SwitchStatement, pos)
{
	Condition = cond;
	Content = std::move(content);
}

FxSwitchStatement::~FxSwitchStatement()
{
	SAFE_DELETE(Condition);
}

FxExpression *FxSwitchStatement::Resolve(FCompileContext &ctx)
{
	CHECKRESOLVED();
	SAFE_RESOLVE(Condition, ctx);

	if (Condition->ValueType != TypeName)
	{
		Condition = new FxIntCast(Condition, false);
		SAFE_RESOLVE(Condition, ctx);
	}

	if (Content.Size() == 0)
	{
		ScriptPosition.Message(MSG_WARNING, "Empty switch statement");
		if (Condition->isConstant())
		{
			return new FxNop(ScriptPosition);
		}
		else
		{
			// The condition may have a side effect so it should be processed (possible to-do: Analyze all nodes in there and delete if not.)
			auto x = Condition;
			Condition = nullptr;
			delete this;
			x->NeedResult = false;
			return x;
		}
	}

	auto outerctrl = ctx.ControlStmt;
	ctx.ControlStmt = this;

	for (auto &line : Content)
	{
		SAFE_RESOLVE(line, ctx);
		line->NeedResult = false;
	}
	ctx.ControlStmt = outerctrl;

	if (Condition->isConstant())
	{
		ScriptPosition.Message(MSG_WARNING, "Case expression is constant");
		auto &content = Content;
		int defaultindex = -1;
		int defaultbreak = -1;
		int caseindex = -1;
		int casebreak = -1;
		// look for a case label with a matching value
		for (unsigned i = 0; i < content.Size(); i++)
		{
			if (content[i] != nullptr)
			{
				if (content[i]->ExprType == EFX_CaseStatement)
				{
					auto casestmt = static_cast<FxCaseStatement *>(content[i]);
					if (casestmt->Condition == nullptr) defaultindex = i;
					else if (casestmt->CaseValue == static_cast<FxConstant *>(Condition)->GetValue().GetInt()) caseindex = i;
					if (casestmt->Condition && casestmt->Condition->ValueType != Condition->ValueType)
					{
						casestmt->Condition->ScriptPosition.Message(MSG_ERROR, "Type mismatch in case statement");
						delete this;
						return nullptr;
					}
				}
				if (content[i]->ExprType == EFX_JumpStatement && static_cast<FxJumpStatement *>(content[i])->Token == TK_Break)
				{
					if (defaultindex >= 0 && defaultbreak < 0) defaultbreak = i;
					if (caseindex >= 0 && casebreak < 0)
					{
						casebreak = i;
						break;	// when we find this we do not need to look any further.
					}
				}
			}
		}
		if (caseindex < 0)
		{
			caseindex = defaultindex;
			casebreak = defaultbreak;
		}
		if (caseindex > 0 && casebreak - caseindex > 1)
		{
			auto seq = new FxSequence(ScriptPosition);
			for (int i = caseindex + 1; i < casebreak; i++)
			{
				if (content[i] != nullptr && content[i]->ExprType != EFX_CaseStatement)
				{
					seq->Add(content[i]);
					content[i] = nullptr;
				}
			}
			delete this;
			return seq->Resolve(ctx);
		}
		delete this;
		return new FxNop(ScriptPosition);
	}

	int mincase = INT_MAX;
	int maxcase = INT_MIN;
	for (auto line : Content)
	{
		if (line->ExprType == EFX_CaseStatement)
		{
			auto casestmt = static_cast<FxCaseStatement *>(line);
			if (casestmt->Condition != nullptr)
			{
				CaseAddr ca = { casestmt->CaseValue, 0 };
				CaseAddresses.Push(ca);
				if (ca.casevalue < mincase) mincase = ca.casevalue;
				if (ca.casevalue > maxcase) maxcase = ca.casevalue;
			}
		}
	}
	return this;
}

ExpEmit FxSwitchStatement::Emit(VMFunctionBuilder *build)
{
	assert(Condition != nullptr);
	ExpEmit emit = Condition->Emit(build);
	assert(emit.RegType == REGT_INT);
	// todo: 
	// - sort jump table by value.
	// - optimize the switch dispatcher to run in native code instead of executing each single branch instruction on its own.
	// e.g.: build->Emit(OP_SWITCH, emit.RegNum, build->GetConstantInt(CaseAddresses.Size());
	for (auto &ca : CaseAddresses)
	{
		if (ca.casevalue >= 0 && ca.casevalue <= 0xffff)
		{
			build->Emit(OP_TEST, emit.RegNum, (VM_SHALF)ca.casevalue);
		}
		else if (ca.casevalue < 0 && ca.casevalue >= -0xffff)
		{
			build->Emit(OP_TESTN, emit.RegNum, (VM_SHALF)-ca.casevalue);
		}
		else
		{
			build->Emit(OP_EQ_K, 1, emit.RegNum, build->GetConstantInt(ca.casevalue));
		}
		ca.jumpaddress = build->Emit(OP_JMP, 0);
	}
	size_t DefaultAddress = build->Emit(OP_JMP, 0);
	bool defaultset = false;

	for (auto line : Content)
	{
		switch (line->ExprType)
		{
		case EFX_CaseStatement:
			if (static_cast<FxCaseStatement *>(line)->Condition != nullptr)
			{
				for (auto &ca : CaseAddresses)
				{
					if (ca.casevalue == static_cast<FxCaseStatement *>(line)->CaseValue)
					{
						build->BackpatchToHere(ca.jumpaddress);
						break;
					}
				}
			}
			else
			{
				build->BackpatchToHere(DefaultAddress);
				defaultset = true;
			}
			break;

		default:
			line->Emit(build);
			break;
		}
	}
	for (auto addr : Breaks)
	{
		build->BackpatchToHere(addr->Address);
	}
	if (!defaultset) build->BackpatchToHere(DefaultAddress);
	Content.DeleteAndClear();
	Content.ShrinkToFit();
	return ExpEmit();
}

//==========================================================================
//
// FxSequence :: CheckReturn
//
//==========================================================================

bool FxSwitchStatement::CheckReturn()
{
	//A switch statement returns when it contains no breaks and ends with a return
	for (auto line : Content)
	{
		if (line->ExprType == EFX_JumpStatement)
		{
			return false;	// Break means that the end of the statement will be reached, Continue cannot happen in the last statement of the last block.
		}
	}
	return Content.Size() > 0 && Content.Last()->CheckReturn();
}

//==========================================================================
//
// FxCaseStatement
//
//==========================================================================

FxCaseStatement::FxCaseStatement(FxExpression *cond, const FScriptPosition &pos)
	: FxExpression(EFX_CaseStatement, pos)
{
	Condition = cond;
}

FxCaseStatement::~FxCaseStatement()
{
	SAFE_DELETE(Condition);
}

FxExpression *FxCaseStatement::Resolve(FCompileContext &ctx)
{
	CHECKRESOLVED();
	SAFE_RESOLVE_OPT(Condition, ctx);

	if (Condition != nullptr)
	{
		if (!Condition->isConstant())
		{
			ScriptPosition.Message(MSG_ERROR, "Case label must be a constant value");
			delete this;
			return nullptr;
		}
		// Case labels can be ints or names.
		if (Condition->ValueType != TypeName)
		{
			Condition = new FxIntCast(Condition, false);
			SAFE_RESOLVE(Condition, ctx);
			CaseValue = static_cast<FxConstant *>(Condition)->GetValue().GetInt();
		}
		else
		{
			CaseValue = static_cast<FxConstant *>(Condition)->GetValue().GetName();
		}
	}
	return this;
}

//==========================================================================
//
// FxIfStatement
//
//==========================================================================

FxIfStatement::FxIfStatement(FxExpression *cond, FxExpression *true_part,
	FxExpression *false_part, const FScriptPosition &pos)
: FxExpression(EFX_IfStatement, pos)
{
	Condition = cond;
	WhenTrue = true_part;
	WhenFalse = false_part;
	if (WhenTrue != nullptr) WhenTrue->NeedResult = false;
	if (WhenFalse != nullptr) WhenFalse->NeedResult = false;
	assert(cond != nullptr);
}

FxIfStatement::~FxIfStatement()
{
	SAFE_DELETE(Condition);
	SAFE_DELETE(WhenTrue);
	SAFE_DELETE(WhenFalse);
}

FxExpression *FxIfStatement::Resolve(FCompileContext &ctx)
{
	CHECKRESOLVED();

	if (WhenTrue == nullptr && WhenFalse == nullptr)
	{ // We don't do anything either way, so disappear
		delete this;
		ScriptPosition.Message(MSG_WARNING, "empty if statement");
		return new FxNop(ScriptPosition);
	}

	SAFE_RESOLVE(Condition, ctx);

	if (Condition->ValueType != TypeBool)
	{
		Condition = new FxBoolCast(Condition, false);
		SAFE_RESOLVE(Condition, ctx);
	}

	if (WhenTrue != nullptr)
	{
		WhenTrue = WhenTrue->Resolve(ctx);
		ABORT(WhenTrue);
	}
	if (WhenFalse != nullptr)
	{
		WhenFalse = WhenFalse->Resolve(ctx);
		ABORT(WhenFalse);
	}

	ValueType = TypeVoid;

	if (Condition->isConstant())
	{
		ExpVal condval = static_cast<FxConstant *>(Condition)->GetValue();
		bool result = condval.GetBool();

		FxExpression *e = result ? WhenTrue : WhenFalse;
		delete (result ? WhenFalse : WhenTrue);
		WhenTrue = WhenFalse = nullptr;
		if (e == nullptr) e = new FxNop(ScriptPosition);	// create a dummy if this statement gets completely removed by optimizing out the constant parts.
		delete this;
		return e;
	}

	return this;
}

ExpEmit FxIfStatement::Emit(VMFunctionBuilder *build)
{
	ExpEmit v;
	size_t jumpspot;
	FxExpression *path1, *path2;
	int condcheck;

	// This is pretty much copied from FxConditional, except we don't
	// keep any results.
	ExpEmit cond = Condition->Emit(build);
	assert(cond.RegType != REGT_STRING && !cond.Konst);

	if (WhenTrue != nullptr)
	{
		path1 = WhenTrue;
		path2 = WhenFalse;
		condcheck = 1;
	}
	else
	{
		// When there is only a false path, reverse the condition so we can
		// treat it as a true path.
		assert(WhenFalse != nullptr);
		path1 = WhenFalse;
		path2 = nullptr;
		condcheck = 0;
	}

	// Test condition.

	switch (cond.RegType)
	{
	default:
	case REGT_INT:
		build->Emit(OP_EQ_K, condcheck, cond.RegNum, build->GetConstantInt(0));
		break;

	case REGT_FLOAT:
		build->Emit(OP_EQF_K, condcheck, cond.RegNum, build->GetConstantFloat(0));
		break;

	case REGT_POINTER:
		build->Emit(OP_EQA_K, condcheck, cond.RegNum, build->GetConstantAddress(nullptr, ATAG_GENERIC));
		break;
	}
	jumpspot = build->Emit(OP_JMP, 0);
	cond.Free(build);

	// Evaluate first path
	v = path1->Emit(build);
	v.Free(build);
	if (path2 != nullptr)
	{
		size_t path1jump;
		
		// if the branch ends with a return we do not need a terminating jmp.
		if (!path1->CheckReturn()) path1jump = build->Emit(OP_JMP, 0);
		else path1jump = 0xffffffff;
		// Evaluate second path
		build->BackpatchToHere(jumpspot);
		v = path2->Emit(build);
		v.Free(build);
		jumpspot = path1jump;
	}
	if (jumpspot != 0xffffffff) build->BackpatchToHere(jumpspot);
	return ExpEmit();
}


//==========================================================================
//
// FxIfStatement :: CheckReturn
//
//==========================================================================

bool FxIfStatement::CheckReturn()
{
	//An if statement returns if both branches return. Both branches must be present.
	return WhenTrue != nullptr && WhenTrue->CheckReturn() &&
			WhenFalse != nullptr && WhenFalse->CheckReturn();
}

//==========================================================================
//
// FxLoopStatement :: Resolve
//
// saves the loop pointer in the context and sets this object as the current loop
// so that continues and breaks always resolve to the innermost loop.
//
//==========================================================================

FxExpression *FxLoopStatement::Resolve(FCompileContext &ctx)
{
	auto outerctrl = ctx.ControlStmt;
	auto outer = ctx.Loop;
	ctx.ControlStmt = this;
	ctx.Loop = this;
	auto x = DoResolve(ctx);
	ctx.Loop = outer;
	ctx.ControlStmt = outerctrl;
	return x;
}

void FxLoopStatement::Backpatch(VMFunctionBuilder *build, size_t loopstart, size_t loopend)
{
	// Give a proper address to any break/continue statement within this loop.
	for (unsigned int i = 0; i < Jumps.Size(); i++)
	{
		if (Jumps[i]->Token == TK_Break)
		{
			build->Backpatch(Jumps[i]->Address, loopend);
		}
		else
		{ // Continue statement.
			build->Backpatch(Jumps[i]->Address, loopstart);
		}
	}
}

//==========================================================================
//
// FxWhileLoop
//
//==========================================================================

FxWhileLoop::FxWhileLoop(FxExpression *condition, FxExpression *code, const FScriptPosition &pos)
: FxLoopStatement(EFX_WhileLoop, pos), Condition(condition), Code(code)
{
	ValueType = TypeVoid;
}

FxWhileLoop::~FxWhileLoop()
{
	SAFE_DELETE(Condition);
	SAFE_DELETE(Code);
}

FxExpression *FxWhileLoop::DoResolve(FCompileContext &ctx)
{
	CHECKRESOLVED();
	SAFE_RESOLVE(Condition, ctx);
	SAFE_RESOLVE_OPT(Code, ctx);

	if (Condition->ValueType != TypeBool)
	{
		Condition = new FxBoolCast(Condition);
		SAFE_RESOLVE(Condition, ctx);
	}

	if (Condition->isConstant())
	{
		if (static_cast<FxConstant *>(Condition)->GetValue().GetBool() == false)
		{ // Nothing happens
			FxExpression *nop = new FxNop(ScriptPosition);
			delete this;
			return nop;
		}
		else if (Code == nullptr)
		{ // "while (true) { }"
		  // Someone could be using this for testing.
			ScriptPosition.Message(MSG_WARNING, "Infinite empty loop");
		}
	}

	return this;
}

ExpEmit FxWhileLoop::Emit(VMFunctionBuilder *build)
{
	assert(Condition->ValueType == TypeBool);

	size_t loopstart, loopend;
	size_t jumpspot;

	// Evaluate the condition and execute/break out of the loop.
	loopstart = build->GetAddress();
	if (!Condition->isConstant())
	{
		ExpEmit cond = Condition->Emit(build);
		build->Emit(OP_TEST, cond.RegNum, 0);
		jumpspot = build->Emit(OP_JMP, 0);
		cond.Free(build);
	}
	else assert(static_cast<FxConstant *>(Condition)->GetValue().GetBool() == true);

	// Execute the loop's content.
	if (Code != nullptr)
	{
		ExpEmit code = Code->Emit(build);
		code.Free(build);
	}

	// Loop back.
	build->Backpatch(build->Emit(OP_JMP, 0), loopstart);
	loopend = build->GetAddress();

	if (!Condition->isConstant()) 
	{
		build->Backpatch(jumpspot, loopend);
	}

	Backpatch(build, loopstart, loopend);
	return ExpEmit();
}

//==========================================================================
//
// FxDoWhileLoop
//
//==========================================================================

FxDoWhileLoop::FxDoWhileLoop(FxExpression *condition, FxExpression *code, const FScriptPosition &pos)
: FxLoopStatement(EFX_DoWhileLoop, pos), Condition(condition), Code(code)
{
	ValueType = TypeVoid;
}

FxDoWhileLoop::~FxDoWhileLoop()
{
	SAFE_DELETE(Condition);
	SAFE_DELETE(Code);
}

FxExpression *FxDoWhileLoop::DoResolve(FCompileContext &ctx)
{
	CHECKRESOLVED();
	SAFE_RESOLVE(Condition, ctx);
	SAFE_RESOLVE_OPT(Code, ctx);

	if (Condition->ValueType != TypeBool)
	{
		Condition = new FxBoolCast(Condition);
		SAFE_RESOLVE(Condition, ctx);
	}

	if (Condition->isConstant())
	{
		if (static_cast<FxConstant *>(Condition)->GetValue().GetBool() == false)
		{ // The code executes once, if any.
			if (Jumps.Size() == 0)
			{ // We would still have to handle the jumps however.
				FxExpression *e = Code;
				if (e == nullptr) e = new FxNop(ScriptPosition);
				Code = nullptr;
				delete this;
				return e;
			}
		}
		else if (Code == nullptr)
		{ // "do { } while (true);"
		  // Someone could be using this for testing.
			ScriptPosition.Message(MSG_WARNING, "Infinite empty loop");
		}
	}

	return this;
}

ExpEmit FxDoWhileLoop::Emit(VMFunctionBuilder *build)
{
	assert(Condition->ValueType == TypeBool);

	size_t loopstart, loopend;
	size_t codestart;

	// Execute the loop's content.
	codestart = build->GetAddress();
	if (Code != nullptr)
	{
		ExpEmit code = Code->Emit(build);
		code.Free(build);
	}

	// Evaluate the condition and execute/break out of the loop.
	loopstart = build->GetAddress();
	if (!Condition->isConstant())
	{
		ExpEmit cond = Condition->Emit(build);
		build->Emit(OP_TEST, cond.RegNum, 1);
		cond.Free(build);
		build->Backpatch(build->Emit(OP_JMP, 0), codestart);
	}
	else if (static_cast<FxConstant *>(Condition)->GetValue().GetBool() == true)
	{ // Always looping
		build->Backpatch(build->Emit(OP_JMP, 0), codestart);
	}
	loopend = build->GetAddress();

	Backpatch(build, loopstart, loopend);

	return ExpEmit();
}

//==========================================================================
//
// FxForLoop
//
//==========================================================================

FxForLoop::FxForLoop(FxExpression *init, FxExpression *condition, FxExpression *iteration, FxExpression *code, const FScriptPosition &pos)
: FxLoopStatement(EFX_ForLoop, pos), Init(init), Condition(condition), Iteration(iteration), Code(code)
{
	ValueType = TypeVoid;
	if (Iteration != nullptr) Iteration->NeedResult = false;
	if (Code != nullptr) Code->NeedResult = false;
}

FxForLoop::~FxForLoop()
{
	SAFE_DELETE(Init);
	SAFE_DELETE(Condition);
	SAFE_DELETE(Iteration);
	SAFE_DELETE(Code);
}

FxExpression *FxForLoop::DoResolve(FCompileContext &ctx)
{
	CHECKRESOLVED();
	SAFE_RESOLVE_OPT(Init, ctx);
	SAFE_RESOLVE_OPT(Condition, ctx);
	SAFE_RESOLVE_OPT(Iteration, ctx);
	SAFE_RESOLVE_OPT(Code, ctx);

	if (Condition != nullptr)
	{
		if (Condition->ValueType != TypeBool)
		{
			Condition = new FxBoolCast(Condition);
			SAFE_RESOLVE(Condition, ctx);
		}

		if (Condition->isConstant())
		{
			if (static_cast<FxConstant *>(Condition)->GetValue().GetBool() == false)
			{ // Nothing happens
				FxExpression *nop = new FxNop(ScriptPosition);
				delete this;
				return nop;
			}
			else
			{ // "for (..; true; ..)"
				delete Condition;
				Condition = nullptr;
			}
		}
	}
	if (Condition == nullptr && Code == nullptr)
	{ // "for (..; ; ..) { }"
	  // Someone could be using this for testing.
		ScriptPosition.Message(MSG_WARNING, "Infinite empty loop");
	}

	return this;
}

ExpEmit FxForLoop::Emit(VMFunctionBuilder *build)
{
	assert((Condition && Condition->ValueType == TypeBool && !Condition->isConstant()) || Condition == nullptr);

	size_t loopstart, loopend;
	size_t codestart;
	size_t jumpspot;

	// Init statement (only used by DECORATE. ZScript is pulling it before the loop statement and enclosing the entire loop in a compound statement so that Init can have local variables.)
	if (Init != nullptr)
	{
		ExpEmit init = Init->Emit(build);
		init.Free(build);
	}

	// Evaluate the condition and execute/break out of the loop.
	codestart = build->GetAddress();
	if (Condition != nullptr)
	{
		ExpEmit cond = Condition->Emit(build);
		build->Emit(OP_TEST, cond.RegNum, 0);
		cond.Free(build);
		jumpspot = build->Emit(OP_JMP, 0);
	}

	// Execute the loop's content.
	if (Code != nullptr)
	{
		ExpEmit code = Code->Emit(build);
		code.Free(build);
	}

	// Iteration statement.
	loopstart = build->GetAddress();
	if (Iteration != nullptr)
	{
		ExpEmit iter = Iteration->Emit(build);
		iter.Free(build);
	}
	build->Backpatch(build->Emit(OP_JMP, 0), codestart);

	// End of loop.
	loopend = build->GetAddress();
	if (Condition != nullptr)
	{
		build->Backpatch(jumpspot, loopend);
	}

	Backpatch(build, loopstart, loopend);
	return ExpEmit();
}

//==========================================================================
//
// FxJumpStatement
//
//==========================================================================

FxJumpStatement::FxJumpStatement(int token, const FScriptPosition &pos)
: FxExpression(EFX_JumpStatement, pos), Token(token)
{
	ValueType = TypeVoid;
}

FxExpression *FxJumpStatement::Resolve(FCompileContext &ctx)
{
	CHECKRESOLVED();

	if (ctx.ControlStmt != nullptr)
	{
		if (ctx.ControlStmt == ctx.Loop || Token == TK_Continue)
		{
			ctx.Loop->Jumps.Push(this);
		}
		else
		{
			// break in switch.
			static_cast<FxSwitchStatement*>(ctx.ControlStmt)->Breaks.Push(this);
		}
		return this;
	}
	else
	{
		ScriptPosition.Message(MSG_ERROR, "'%s' outside of a loop", Token == TK_Break ? "break" : "continue");
		delete this;
		return nullptr;
	}
}

ExpEmit FxJumpStatement::Emit(VMFunctionBuilder *build)
{
	Address = build->Emit(OP_JMP, 0);

	return ExpEmit();
}

//==========================================================================
//
//==========================================================================

FxReturnStatement::FxReturnStatement(FxExpression *value, const FScriptPosition &pos)
: FxExpression(EFX_ReturnStatement, pos), Value(value)
{
	ValueType = TypeVoid;
}

FxReturnStatement::~FxReturnStatement()
{
	SAFE_DELETE(Value);
}

FxExpression *FxReturnStatement::Resolve(FCompileContext &ctx)
{
	CHECKRESOLVED();
	SAFE_RESOLVE_OPT(Value, ctx);

	PPrototype *retproto;
	if (Value == nullptr)
	{
		TArray<PType *> none(0);
		retproto = NewPrototype(none, none);
	}
	else
	{
		// If we already know the real return type we need at least try to cast the value to its proper type (unless in an anonymous function.)
		if (ctx.ReturnProto != nullptr && ctx.ReturnProto->ReturnTypes.Size() > 0 && ctx.Function->SymbolName != NAME_None)
		{
			Value = new FxTypeCast(Value, ctx.ReturnProto->ReturnTypes[0], false, false);
			Value = Value->Resolve(ctx);
			ABORT(Value);
		}
		retproto = Value->ReturnProto();
	}

	ctx.CheckReturn(retproto, ScriptPosition);

	return this;
}

ExpEmit FxReturnStatement::Emit(VMFunctionBuilder *build)
{
	ExpEmit out(0, REGT_NIL);

	// If we return nothing, use a regular RET opcode.
	// Otherwise just return the value we're given.
	if (Value == nullptr)
	{
		build->Emit(OP_RET, RET_FINAL, REGT_NIL, 0);
	}
	else
	{
		out = Value->Emit(build);

		// Check if it is a function call that simplified itself
		// into a tail call in which case we don't emit anything.
		if (!out.Final)
		{
			if (Value->ValueType == TypeVoid)
			{ // Nothing is returned.
				build->Emit(OP_RET, RET_FINAL, REGT_NIL, 0);
			}
			else
			{
				build->Emit(OP_RET, RET_FINAL, EncodeRegType(out), out.RegNum);
			}
		}
	}

	out.Final = true;
	return out;
}

VMFunction *FxReturnStatement::GetDirectFunction()
{
	if (Value != nullptr)
	{
		return Value->GetDirectFunction();
	}
	return nullptr;
}

//==========================================================================
//
//==========================================================================

FxClassTypeCast::FxClassTypeCast(PClassPointer *dtype, FxExpression *x)
: FxExpression(EFX_ClassTypeCast, x->ScriptPosition)
{
	ValueType = dtype;
	desttype = dtype->ClassRestriction;
	basex=x;
}

//==========================================================================
//
//
//
//==========================================================================

FxClassTypeCast::~FxClassTypeCast()
{
	SAFE_DELETE(basex);
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxClassTypeCast::Resolve(FCompileContext &ctx)
{
	CHECKRESOLVED();
	SAFE_RESOLVE(basex, ctx);

	if (basex->ValueType == TypeNullPtr)
	{
		basex->ValueType = ValueType;
		auto x = basex;
		basex = nullptr;
		delete this;
		return x;
	}
	auto to = static_cast<PClassPointer *>(ValueType);
	if (basex->ValueType->GetClass() == RUNTIME_CLASS(PClassPointer))
	{
		auto from = static_cast<PClassPointer *>(basex->ValueType);
		if (from->ClassRestriction->IsDescendantOf(to->ClassRestriction))
		{
			basex->ValueType = to;
			auto x = basex;
			basex = nullptr;
			delete this;
			return x;
		}
		ScriptPosition.Message(MSG_ERROR, "Cannot convert from %s to %s: Incompatible class types", from->ClassRestriction->TypeName.GetChars(), to->ClassRestriction->TypeName.GetChars());
		delete this;
		return nullptr;
	}
	
	if (basex->ValueType != TypeName && basex->ValueType != TypeString)
	{
		ScriptPosition.Message(MSG_ERROR, "Cannot convert %s to class type", basex->ValueType->DescriptiveName());
		delete this;
		return nullptr;
	}

	if (basex->isConstant())
	{
		FName clsname = static_cast<FxConstant *>(basex)->GetValue().GetName();
		PClass *cls = nullptr;

		if (clsname != NAME_None)
		{
			cls = PClass::FindClass(clsname);
			if (cls == nullptr)
			{
				/* lax */
				// Since this happens in released WADs it must pass without a terminal error... :(
				ScriptPosition.Message(MSG_OPTERROR,
					"Unknown class name '%s'",
					clsname.GetChars(), desttype->TypeName.GetChars());
			}
			else
			{
				if (!cls->IsDescendantOf(desttype))
				{
					ScriptPosition.Message(MSG_OPTERROR, "class '%s' is not compatible with '%s'", clsname.GetChars(), desttype->TypeName.GetChars());
					cls = nullptr;
				}
				else ScriptPosition.Message(MSG_DEBUGLOG, "resolving '%s' as class name", clsname.GetChars());
			}
		}
		FxExpression *x = new FxConstant(cls, to, ScriptPosition);
		delete this;
		return x;
	}
	if (basex->ValueType == TypeString)
	{
		basex = new FxNameCast(basex);
	}
	return this;
}

//==========================================================================
//
// 
//
//==========================================================================

int BuiltinNameToClass(VMValue *param, TArray<VMValue> &defaultparam, int numparam, VMReturn *ret, int numret)
{
	assert(numparam == 2);
	assert(numret == 1);
	assert(param[0].Type == REGT_INT);
	assert(param[1].Type == REGT_POINTER);
	assert(ret->RegType == REGT_POINTER);

	FName clsname = ENamedName(param[0].i);
	if (clsname != NAME_None)
	{
		const PClass *cls = PClass::FindClass(clsname);
		const PClass *desttype = reinterpret_cast<PClass *>(param[1].a);

		if (!cls->IsDescendantOf(desttype))
		{
			// Let the caller check this. The message can be enabled for diagnostic purposes.
			DPrintf(DMSG_SPAMMY, "class '%s' is not compatible with '%s'\n", clsname.GetChars(), desttype->TypeName.GetChars());
			cls = nullptr;
		}
		ret->SetPointer(const_cast<PClass *>(cls), ATAG_OBJECT);
	}
	else ret->SetPointer(nullptr, ATAG_OBJECT);
	return 1;
}

ExpEmit FxClassTypeCast::Emit(VMFunctionBuilder *build)
{
	if (basex->ValueType != TypeName)
	{
		return ExpEmit(build->GetConstantAddress(nullptr, ATAG_OBJECT), REGT_POINTER, true);
	}
	ExpEmit clsname = basex->Emit(build);
	assert(!clsname.Konst);
	ExpEmit dest(build, REGT_POINTER);
	build->Emit(OP_PARAM, 0, clsname.RegType, clsname.RegNum);
	build->Emit(OP_PARAM, 0, REGT_POINTER | REGT_KONST, build->GetConstantAddress(const_cast<PClass *>(desttype), ATAG_OBJECT));

	// Call the BuiltinNameToClass function to convert from 'name' to class.
	VMFunction *callfunc;
	PSymbol *sym = FindBuiltinFunction(NAME_BuiltinNameToClass, BuiltinNameToClass);

	assert(sym->IsKindOf(RUNTIME_CLASS(PSymbolVMFunction)));
	assert(((PSymbolVMFunction *)sym)->Function != nullptr);
	callfunc = ((PSymbolVMFunction *)sym)->Function;

	build->Emit(OP_CALL_K, build->GetConstantAddress(callfunc, ATAG_OBJECT), 2, 1);
	build->Emit(OP_RESULT, 0, REGT_POINTER, dest.RegNum);
	clsname.Free(build);
	return dest;
}

//==========================================================================
//
//==========================================================================

FxClassPtrCast::FxClassPtrCast(PClass *dtype, FxExpression *x)
	: FxExpression(EFX_ClassPtrCast, x->ScriptPosition)
{
	ValueType = NewClassPointer(dtype);
	desttype = dtype;
	basex = x;
}

//==========================================================================
//
//
//
//==========================================================================

FxClassPtrCast::~FxClassPtrCast()
{
	SAFE_DELETE(basex);
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxClassPtrCast::Resolve(FCompileContext &ctx)
{
	CHECKRESOLVED();
	SAFE_RESOLVE(basex, ctx);

	if (basex->ValueType == TypeNullPtr)
	{
		basex->ValueType = ValueType;
		auto x = basex;
		basex = nullptr;
		delete this;
		return x;
	}
	auto to = static_cast<PClassPointer *>(ValueType);
	if (basex->ValueType->GetClass() == RUNTIME_CLASS(PClassPointer))
	{
		auto from = static_cast<PClassPointer *>(basex->ValueType);
		// Downcast is always ok.
		if (from->ClassRestriction->IsDescendantOf(to->ClassRestriction))
		{
			basex->ValueType = to;
			auto x = basex;
			basex = nullptr;
			delete this;
			return x;
		}
		// Upcast needs a runtime check.
		else if (to->ClassRestriction->IsDescendantOf(from->ClassRestriction))
		{
			return this;
		}
	}
	else if (basex->ValueType == TypeString || basex->ValueType == TypeName)
	{
		FxExpression *x = new FxClassTypeCast(to, basex);
		basex = nullptr;
		delete this;
		return x->Resolve(ctx);
	}
	// Everything else is an error.
	ScriptPosition.Message(MSG_ERROR, "Cannot cast %s to %s. The types are incompatible.", basex->ValueType->DescriptiveName(), to->DescriptiveName());
	delete this;
	return nullptr;
}

//==========================================================================
//
// 
//
//==========================================================================

int BuiltinClassCast(VMValue *param, TArray<VMValue> &defaultparam, int numparam, VMReturn *ret, int numret)
{
	PARAM_PROLOGUE;
	PARAM_CLASS(from, DObject);
	PARAM_CLASS(to, DObject);
	ACTION_RETURN_OBJECT(from->IsDescendantOf(to) ? from : nullptr);
}

ExpEmit FxClassPtrCast::Emit(VMFunctionBuilder *build)
{
	ExpEmit clsname = basex->Emit(build);

	build->Emit(OP_PARAM, 0, clsname.RegType, clsname.RegNum);
	build->Emit(OP_PARAM, 0, REGT_POINTER | REGT_KONST, build->GetConstantAddress(desttype, ATAG_OBJECT));

	VMFunction *callfunc;
	PSymbol *sym = FindBuiltinFunction(NAME_BuiltinClassCast, BuiltinClassCast);

	assert(sym->IsKindOf(RUNTIME_CLASS(PSymbolVMFunction)));
	assert(((PSymbolVMFunction *)sym)->Function != nullptr);
	callfunc = ((PSymbolVMFunction *)sym)->Function;
	clsname.Free(build);
	ExpEmit dest(build, REGT_POINTER);
	build->Emit(OP_CALL_K, build->GetConstantAddress(callfunc, ATAG_OBJECT), 2, 1);
	build->Emit(OP_RESULT, 0, REGT_POINTER, dest.RegNum);
	return dest;
}

//==========================================================================
//
// Symbolic state labels. 
// Conversion will not happen inside the compiler anymore because it causes
// just too many problems.
//
//==========================================================================

FxExpression *FxStateByIndex::Resolve(FCompileContext &ctx)
{
	CHECKRESOLVED();
	ABORT(ctx.Class);
	auto aclass = dyn_cast<PClassActor>(ctx.Class);

	// This expression type can only be used from actors, for everything else it has already produced a compile error.
	assert(aclass != nullptr && aclass->NumOwnedStates > 0);

	if (aclass->NumOwnedStates <= index)
	{
		ScriptPosition.Message(MSG_ERROR, "%s: Attempt to jump to non existing state index %d", 
			ctx.Class->TypeName.GetChars(), index);
		delete this;
		return nullptr;
	}
	int symlabel = StateLabels.AddPointer(aclass->OwnedStates + index);
	FxExpression *x = new FxConstant(symlabel, ScriptPosition);
	x->ValueType = TypeStateLabel;
	delete this;
	return x;
}

//==========================================================================
//
//
//
//==========================================================================

FxRuntimeStateIndex::FxRuntimeStateIndex(FxExpression *index)
: FxExpression(EFX_RuntimeStateIndex, index->ScriptPosition), Index(index)
{
	ValueType = TypeStateLabel;
}

FxRuntimeStateIndex::~FxRuntimeStateIndex()
{
	SAFE_DELETE(Index);
}

FxExpression *FxRuntimeStateIndex::Resolve(FCompileContext &ctx)
{
	CHECKRESOLVED();
	SAFE_RESOLVE(Index, ctx);

	if (!Index->IsNumeric())
	{
		ScriptPosition.Message(MSG_ERROR, "Numeric type expected");
		delete this;
		return nullptr;
	}
	else if (Index->isConstant())
	{
		int index = static_cast<FxConstant *>(Index)->GetValue().GetInt();
		if (index < 0 || (index == 0 && !ctx.FromDecorate))
		{
			ScriptPosition.Message(MSG_ERROR, "State index must be positive");
			delete this;
			return nullptr;
		}
		else if (index == 0)
		{
			int symlabel = StateLabels.AddPointer(nullptr);
			auto x = new FxConstant(symlabel, ScriptPosition);
			delete this;
			x->ValueType = TypeStateLabel;
			return x;
		}
		else
		{
			auto x = new FxStateByIndex(ctx.StateIndex + index, ScriptPosition);
			delete this;
			return x->Resolve(ctx);
		}
	}
	else if (Index->ValueType->GetRegType() != REGT_INT)
	{ // Float.
		Index = new FxIntCast(Index, ctx.FromDecorate);
		SAFE_RESOLVE(Index, ctx);
	}
	auto aclass = dyn_cast<PClassActor>(ctx.Class);
	assert(aclass != nullptr && aclass->NumOwnedStates > 0);
	symlabel = StateLabels.AddPointer(aclass->OwnedStates + ctx.StateIndex);
	ValueType = TypeStateLabel;
	return this;
}

ExpEmit FxRuntimeStateIndex::Emit(VMFunctionBuilder *build)
{
	ExpEmit out = Index->Emit(build);
	// out = (clamp(Index, 0, 32767) << 16) | symlabel | 0x80000000;  0x80000000 is here to make it negative.
	build->Emit(OP_MAX_RK, out.RegNum, out.RegNum, build->GetConstantInt(0));
	build->Emit(OP_MIN_RK, out.RegNum, out.RegNum, build->GetConstantInt(32767));
	build->Emit(OP_SLL_RI, out.RegNum, out.RegNum, 16);
	build->Emit(OP_OR_RK, out.RegNum, out.RegNum, build->GetConstantInt(symlabel|0x80000000));
	return out;
}

//==========================================================================
//
//
//
//==========================================================================

FxMultiNameState::FxMultiNameState(const char *_statestring, const FScriptPosition &pos)
	:FxExpression(EFX_MultiNameState, pos)
{
	FName scopename;
	FString statestring = _statestring;
	int scopeindex = statestring.IndexOf("::");

	if (scopeindex >= 0)
	{
		scopename = FName(statestring, scopeindex, false);
		statestring = statestring.Right(statestring.Len() - scopeindex - 2);
	}
	else
	{
		scopename = NAME_None;
	}
	names = MakeStateNameList(statestring);
	names.Insert(0, scopename);
	scope = nullptr;
}

//==========================================================================
//
//
//
//==========================================================================

FxExpression *FxMultiNameState::Resolve(FCompileContext &ctx)
{
	CHECKRESOLVED();
	ABORT(ctx.Class);
	int symlabel;
	auto clstype = dyn_cast<PClassActor>(ctx.Class);

	if (names[0] == NAME_None)
	{
		scope = nullptr;
	}
	else if (clstype == nullptr)
	{
		// not in an actor, so any further checks are pointless.
		ScriptPosition.Message(MSG_ERROR, "'%s' is not an ancestor of '%s'", names[0].GetChars(), ctx.Class->TypeName.GetChars());
		delete this;
		return nullptr;
	}
	else if (names[0] == NAME_Super)
	{
		scope = dyn_cast<PClassActor>(clstype->ParentClass);
	}
	else
	{
		scope = PClass::FindActor(names[0]);
		if (scope == nullptr)
		{
			ScriptPosition.Message(MSG_ERROR, "Unknown class '%s' in state label", names[0].GetChars());
			delete this;
			return nullptr;
		}
		else if (!scope->IsAncestorOf(clstype))
		{
			ScriptPosition.Message(MSG_ERROR, "'%s' is not an ancestor of '%s'", names[0].GetChars(), ctx.Class->TypeName.GetChars());
			delete this;
			return nullptr;
		}
	}
	if (scope != nullptr)
	{
		FState *destination = nullptr;
		// If the label is class specific we can resolve it right here
		if (names[1] != NAME_None)
		{
			destination = scope->FindState(names.Size()-1, &names[1], false);
			if (destination == nullptr)
			{
				ScriptPosition.Message(MSG_OPTERROR, "Unknown state jump destination");
				/* lax */
				return this;
			}
		}
		symlabel = StateLabels.AddPointer(destination);
	}
	else
	{
		names.Delete(0);
		symlabel = StateLabels.AddNames(names);
	}
	FxExpression *x = new FxConstant(symlabel, ScriptPosition);
	x->ValueType = TypeStateLabel;
	delete this;
	return x;
}

//==========================================================================
//
// declares a single local variable (no arrays)
//
//==========================================================================

FxLocalVariableDeclaration::FxLocalVariableDeclaration(PType *type, FName name, FxExpression *initval, int varflags, const FScriptPosition &p)
	:FxExpression(EFX_LocalVariableDeclaration, p)
{
	ValueType = type;
	VarFlags = varflags;
	Name = name;
	RegCount = type == TypeVector2 ? 2 : type == TypeVector3 ? 3 : 1;
	Init = initval;
}

FxLocalVariableDeclaration::~FxLocalVariableDeclaration()
{
	SAFE_DELETE(Init);
}

FxExpression *FxLocalVariableDeclaration::Resolve(FCompileContext &ctx)
{
	CHECKRESOLVED();
	if (ctx.Block == nullptr)
	{
		ScriptPosition.Message(MSG_ERROR, "Variable declaration outside compound statement");
		delete this;
		return nullptr;
	}
	if (ValueType->RegType == REGT_NIL)
	{
		auto sfunc = static_cast<VMScriptFunction *>(ctx.Function->Variants[0].Implementation);
		StackOffset = sfunc->AllocExtraStack(ValueType);
		// Todo: Process the compound initializer once implemented.
	}
	else
	{
		if (Init) Init = new FxTypeCast(Init, ValueType, false);
		SAFE_RESOLVE_OPT(Init, ctx);
	}
	ctx.Block->LocalVars.Push(this);
	return this;
}

void FxLocalVariableDeclaration::SetReg(ExpEmit emit)
{
	assert(ValueType->GetRegType() == emit.RegType && ValueType->GetRegCount() == emit.RegCount);
	RegNum = emit.RegNum;
}

ExpEmit FxLocalVariableDeclaration::Emit(VMFunctionBuilder *build)
{
	if (ValueType->RegType != REGT_NIL)
	{
		if (Init == nullptr)
		{
			if (RegNum == -1)
			{
				if (!(VarFlags & VARF_Out)) RegNum = build->Registers[ValueType->GetRegType()].Get(RegCount);
				else RegNum = build->Registers[REGT_POINTER].Get(1);
			}
		}
		else
		{
			assert(!(VarFlags & VARF_Out));	// 'out' variables should never be initialized, they can only exist as function parameters.
			ExpEmit emitval = Init->Emit(build);

			int regtype = emitval.RegType;
			if (regtype < REGT_INT || regtype > REGT_TYPE)
			{
				ScriptPosition.Message(MSG_ERROR, "Attempted to assign a non-value");
				return ExpEmit();
			}
			if (emitval.Konst)
			{
				auto constval = static_cast<FxConstant *>(Init);
				RegNum = build->Registers[regtype].Get(1);
				switch (regtype)
				{
				default:
				case REGT_INT:
					build->Emit(OP_LK, RegNum, build->GetConstantInt(constval->GetValue().GetInt()));
					break;

				case REGT_FLOAT:
					build->Emit(OP_LKF, RegNum, build->GetConstantFloat(constval->GetValue().GetFloat()));
					break;

				case REGT_POINTER:
				{
					bool isobject = ValueType->IsKindOf(RUNTIME_CLASS(PClassPointer)) || (ValueType->IsKindOf(RUNTIME_CLASS(PPointer)) && static_cast<PPointer*>(ValueType)->PointedType->IsKindOf(RUNTIME_CLASS(PClass)));
					build->Emit(OP_LKP, RegNum, build->GetConstantAddress(constval->GetValue().GetPointer(), isobject ? ATAG_OBJECT : ATAG_GENERIC));
					break;
				}
				case REGT_STRING:
					build->Emit(OP_LKS, RegNum, build->GetConstantString(constval->GetValue().GetString()));
				}
				emitval.Free(build);
			}
			else if (Init->ExprType != EFX_LocalVariable)
			{
				// take over the register that got allocated while emitting the Init expression.
				RegNum = emitval.RegNum;
			}
			else
			{
				ExpEmit out(build, emitval.RegType, emitval.RegCount);
				build->Emit(ValueType->GetMoveOp(), out.RegNum, emitval.RegNum);
				RegNum = out.RegNum;
			}
		}
	}
	else
	{
		// Init arrays and structs.
	}
	return ExpEmit();
}

void FxLocalVariableDeclaration::Release(VMFunctionBuilder *build)
{
	// Release the register after the containing block gets closed
	if(RegNum != -1)
	{
		build->Registers[ValueType->GetRegType()].Return(RegNum, RegCount);
	}
	// Stack space will not be released because that would make controlled destruction impossible.
	// For that all local stack variables need to live for the entire execution of a function.
}


FxStaticArray::FxStaticArray(PType *type, FName name, FArgumentList &args, const FScriptPosition &pos)
	: FxLocalVariableDeclaration(NewArray(type, args.Size()), name, nullptr, VARF_Static|VARF_ReadOnly, pos)
{
	ElementType = type;
	ExprType = EFX_StaticArray;
	values = std::move(args);
}

FxExpression *FxStaticArray::Resolve(FCompileContext &ctx)
{
	bool fail = false;
	for (unsigned i = 0; i < values.Size(); i++)
	{
		values[i] = new FxTypeCast(values[i], ElementType, false);
		values[i] = values[i]->Resolve(ctx);
		if (values[i] == nullptr) fail = true;
		else if (!values[i]->isConstant())
		{
			ScriptPosition.Message(MSG_ERROR, "Initializer must be constant");
			fail = true;
		}
	}
	if (fail)
	{
		delete this;
		return nullptr;
	}
	if (ElementType->GetRegType() == REGT_NIL)
	{
		ScriptPosition.Message(MSG_ERROR, "Invalid type for constant array");
		delete this;
		return nullptr;
	}

	ctx.Block->LocalVars.Push(this);
	return this;
}

ExpEmit FxStaticArray::Emit(VMFunctionBuilder *build)
{
	switch (ElementType->GetRegType())
	{
	default:
		assert(false && "Invalid register type");
		break;

	case REGT_INT:
	{
		TArray<int> cvalues;
		for (auto v : values) cvalues.Push(static_cast<FxConstant *>(v)->GetValue().GetInt());
		StackOffset = build->AllocConstantsInt(cvalues.Size(), &cvalues[0]);
		break;
	}
	case REGT_FLOAT:
	{
		TArray<double> cvalues;
		for (auto v : values) cvalues.Push(static_cast<FxConstant *>(v)->GetValue().GetFloat());
		StackOffset = build->AllocConstantsFloat(cvalues.Size(), &cvalues[0]);
		break;
	}
	case REGT_STRING:
	{
		TArray<FString> cvalues;
		for (auto v : values) cvalues.Push(static_cast<FxConstant *>(v)->GetValue().GetString());
		StackOffset = build->AllocConstantsString(cvalues.Size(), &cvalues[0]);
		break;
	}
	case REGT_POINTER:
	{
		TArray<void*> cvalues;
		for (auto v : values) cvalues.Push(static_cast<FxConstant *>(v)->GetValue().GetPointer());
		StackOffset = build->AllocConstantsAddress(cvalues.Size(), &cvalues[0], ElementType->GetLoadOp() == OP_LO ? ATAG_OBJECT : ATAG_GENERIC);
		break;
	}
	}
	return ExpEmit();
}