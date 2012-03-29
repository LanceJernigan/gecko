/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sw=4 et tw=78:
 *
 * ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is Mozilla Communicator client code, released
 * March 31, 1998.
 *
 * The Initial Developer of the Original Code is
 * Netscape Communications Corporation.
 * Portions created by the Initial Developer are Copyright (C) 1998
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *   Nick Fitzgerald <nfitzgerald@mozilla.com>
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either of the GNU General Public License Version 2 or later (the "GPL"),
 * or the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */

/*
 * JS script operations.
 */

#include <string.h>
#include "jstypes.h"
#include "jsutil.h"
#include "jscrashreport.h"
#include "jsprf.h"
#include "jsapi.h"
#include "jsatom.h"
#include "jscntxt.h"
#include "jsversion.h"
#include "jsdbgapi.h"
#include "jsfun.h"
#include "jsgc.h"
#include "jsgcmark.h"
#include "jsinterp.h"
#include "jslock.h"
#include "jsnum.h"
#include "jsopcode.h"
#include "jsscope.h"
#include "jsscript.h"

#include "frontend/BytecodeEmitter.h"
#include "frontend/Parser.h"
#include "js/MemoryMetrics.h"
#include "methodjit/MethodJIT.h"
#include "methodjit/Retcon.h"
#include "vm/Debugger.h"
#include "vm/Xdr.h"

#include "jsinferinlines.h"
#include "jsinterpinlines.h"
#include "jsobjinlines.h"
#include "jsscriptinlines.h"

using namespace js;
using namespace js::gc;
using namespace js::frontend;

namespace js {

BindingKind
Bindings::lookup(JSContext *cx, JSAtom *name, unsigned *indexp) const
{
    if (!lastBinding)
        return NONE;

    Shape **spp;
    Shape *shape = Shape::search(cx, lastBinding, ATOM_TO_JSID(name), &spp);
    if (!shape)
        return NONE;

    if (indexp)
        *indexp = shape->shortid();

    if (shape->getter() == CallObject::getArgOp)
        return ARGUMENT;

    return shape->writable() ? VARIABLE : CONSTANT;
}

bool
Bindings::add(JSContext *cx, JSAtom *name, BindingKind kind)
{
    if (!ensureShape(cx))
        return false;

    /*
     * We still follow 10.2.3 of ES3 and make argument and variable properties
     * of the Call objects enumerable. ES5 reformulated all of its Clause 10 to
     * avoid objects as activations, something we should do too.
     */
    unsigned attrs = JSPROP_ENUMERATE | JSPROP_PERMANENT;

    uint16_t *indexp;
    PropertyOp getter;
    StrictPropertyOp setter;
    uint32_t slot = CallObject::RESERVED_SLOTS;

    if (kind == ARGUMENT) {
        JS_ASSERT(nvars == 0);
        indexp = &nargs;
        getter = CallObject::getArgOp;
        setter = CallObject::setArgOp;
        slot += nargs;
    } else {
        JS_ASSERT(kind == VARIABLE || kind == CONSTANT);

        indexp = &nvars;
        getter = CallObject::getVarOp;
        setter = CallObject::setVarOp;
        if (kind == CONSTANT)
            attrs |= JSPROP_READONLY;
        slot += nargs + nvars;
    }

    if (*indexp == BINDING_COUNT_LIMIT) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                             (kind == ARGUMENT)
                             ? JSMSG_TOO_MANY_FUN_ARGS
                             : JSMSG_TOO_MANY_LOCALS);
        return false;
    }

    jsid id;
    if (!name) {
        JS_ASSERT(kind == ARGUMENT); /* destructuring */
        id = INT_TO_JSID(nargs);
    } else {
        id = ATOM_TO_JSID(name);
    }

    StackBaseShape base(&CallClass, NULL, BaseShape::VAROBJ);
    base.updateGetterSetter(attrs, getter, setter);

    UnownedBaseShape *nbase = BaseShape::getUnowned(cx, base);
    if (!nbase)
        return NULL;

    StackShape child(nbase, id, slot, 0, attrs, Shape::HAS_SHORTID, *indexp);

    /* Shapes in bindings cannot be dictionaries. */
    Shape *shape = lastBinding->getChildBinding(cx, child);
    if (!shape)
        return false;

    lastBinding = shape;
    ++*indexp;
    return true;
}

Shape *
Bindings::callObjectShape(JSContext *cx) const
{
    if (!hasDup())
        return lastShape();

    /*
     * Build a vector of non-duplicate properties in order from last added
     * to first (i.e., the order we normally have iterate over Shapes). Choose
     * the last added property in each set of dups.
     */
    Vector<const Shape *> shapes(cx);
    HashSet<jsid> seen(cx);
    if (!seen.init())
        return NULL;

    for (Shape::Range r = lastShape()->all(); !r.empty(); r.popFront()) {
        const Shape &s = r.front();
        HashSet<jsid>::AddPtr p = seen.lookupForAdd(s.propid());
        if (!p) {
            if (!seen.add(p, s.propid()))
                return NULL;
            if (!shapes.append(&s))
                return NULL;
        }
    }

    /*
     * Now build the Shape without duplicate properties.
     */
    RootedVarShape shape(cx);
    shape = initialShape(cx);
    for (int i = shapes.length() - 1; i >= 0; --i) {
        shape = shape->getChildBinding(cx, shapes[i]);
        if (!shape)
            return NULL;
    }

    return shape;
}

bool
Bindings::getLocalNameArray(JSContext *cx, Vector<JSAtom *> *namesp)
{
    JS_ASSERT(lastBinding);
    JS_ASSERT(hasLocalNames());

    Vector<JSAtom *> &names = *namesp;
    JS_ASSERT(names.empty());

    unsigned n = countLocalNames();
    if (!names.growByUninitialized(n))
        return false;

#ifdef DEBUG
    JSAtom * const POISON = reinterpret_cast<JSAtom *>(0xdeadbeef);
    for (unsigned i = 0; i < n; i++)
        names[i] = POISON;
#endif

    for (Shape::Range r = lastBinding->all(); !r.empty(); r.popFront()) {
        const Shape &shape = r.front();
        unsigned index = uint16_t(shape.shortid());

        if (shape.getter() == CallObject::getArgOp) {
            JS_ASSERT(index < nargs);
        } else {
            JS_ASSERT(index < nvars);
            index += nargs;
        }

        if (JSID_IS_ATOM(shape.propid())) {
            names[index] = JSID_TO_ATOM(shape.propid());
        } else {
            JS_ASSERT(JSID_IS_INT(shape.propid()));
            JS_ASSERT(shape.getter() == CallObject::getArgOp);
            names[index] = NULL;
        }
    }

#ifdef DEBUG
    for (unsigned i = 0; i < n; i++)
        JS_ASSERT(names[i] != POISON);
#endif

    return true;
}

const Shape *
Bindings::lastArgument() const
{
    JS_ASSERT(lastBinding);

    const js::Shape *shape = lastVariable();
    if (nvars > 0) {
        while (shape->previous() && shape->getter() != CallObject::getArgOp)
            shape = shape->previous();
    }
    return shape;
}

const Shape *
Bindings::lastVariable() const
{
    JS_ASSERT(lastBinding);
    return lastBinding;
}

void
Bindings::makeImmutable()
{
    JS_ASSERT(lastBinding);
    JS_ASSERT(!lastBinding->inDictionary());
}

void
Bindings::trace(JSTracer *trc)
{
    if (lastBinding)
        MarkShape(trc, &lastBinding, "shape");
}

} /* namespace js */

template<XDRMode mode>
static bool
XDRScriptConst(XDRState<mode> *xdr, HeapValue *vp)
{
    /*
     * A script constant can be an arbitrary primitive value as they are used
     * to implement JSOP_LOOKUPSWITCH. But they cannot be objects, see
     * bug 407186.
     */
    enum ConstTag {
        SCRIPT_INT     = 0,
        SCRIPT_DOUBLE  = 1,
        SCRIPT_STRING  = 2,
        SCRIPT_TRUE    = 3,
        SCRIPT_FALSE   = 4,
        SCRIPT_NULL    = 5,
        SCRIPT_VOID    = 6
    };

    uint32_t tag;
    if (mode == XDR_ENCODE) {
        if (vp->isInt32()) {
            tag = SCRIPT_INT;
        } else if (vp->isDouble()) {
            tag = SCRIPT_DOUBLE;
        } else if (vp->isString()) {
            tag = SCRIPT_STRING;
        } else if (vp->isTrue()) {
            tag = SCRIPT_TRUE;
        } else if (vp->isFalse()) {
            tag = SCRIPT_FALSE;
        } else if (vp->isNull()) {
            tag = SCRIPT_NULL;
        } else {
            JS_ASSERT(vp->isUndefined());
            tag = SCRIPT_VOID;
        }
    }

    if (!xdr->codeUint32(&tag))
        return false;

    switch (tag) {
      case SCRIPT_INT: {
        uint32_t i;
        if (mode == XDR_ENCODE)
            i = uint32_t(vp->toInt32());
        if (!xdr->codeUint32(&i))
            return JS_FALSE;
        if (mode == XDR_DECODE)
            vp->init(Int32Value(int32_t(i)));
        break;
      }
      case SCRIPT_DOUBLE: {
        double d;
        if (mode == XDR_ENCODE)
            d = vp->toDouble();
        if (!xdr->codeDouble(&d))
            return false;
        if (mode == XDR_DECODE)
            vp->init(DoubleValue(d));
        break;
      }
      case SCRIPT_STRING: {
        JSString *str;
        if (mode == XDR_ENCODE)
            str = vp->toString();
        if (!xdr->codeString(&str))
            return false;
        if (mode == XDR_DECODE)
            vp->init(StringValue(str));
        break;
      }
      case SCRIPT_TRUE:
        if (mode == XDR_DECODE)
            vp->init(BooleanValue(true));
        break;
      case SCRIPT_FALSE:
        if (mode == XDR_DECODE)
            vp->init(BooleanValue(false));
        break;
      case SCRIPT_NULL:
        if (mode == XDR_DECODE)
            vp->init(NullValue());
        break;
      case SCRIPT_VOID:
        if (mode == XDR_DECODE)
            vp->init(UndefinedValue());
        break;
    }
    return true;
}

template<XDRMode mode>
bool
js::XDRScript(XDRState<mode> *xdr, JSScript **scriptp, JSScript *parentScript)
{
    enum ScriptBits {
        NoScriptRval,
        SavedCallerFun,
        StrictModeCode,
        UsesEval,
        MayNeedArgsObj,
        NeedsArgsObj,
        OwnFilename,
        ParentFilename
    };

    uint32_t length, lineno, nslots;
    uint32_t natoms, nsrcnotes, ntrynotes, nobjects, nregexps, nconsts, i;
    uint32_t prologLength, version, encodedClosedCount;
    uint16_t nClosedArgs = 0, nClosedVars = 0;
    uint32_t nTypeSets = 0;
    uint32_t scriptBits = 0;

    JSContext *cx = xdr->cx();
    JSScript *script;
    nsrcnotes = ntrynotes = natoms = nobjects = nregexps = nconsts = 0;
    jssrcnote *notes = NULL;

    /* XDR arguments, local vars, and upvars. */
    uint16_t nargs, nvars;
#if defined(DEBUG) || defined(__GNUC__) /* quell GCC overwarning */
    script = NULL;
    nargs = nvars = Bindings::BINDING_COUNT_LIMIT;
#endif
    uint32_t argsVars;
    if (mode == XDR_ENCODE) {
        script = *scriptp;
        JS_ASSERT_IF(parentScript, parentScript->compartment() == script->compartment());
    
        /* Should not XDR scripts optimized for a single global object. */
        JS_ASSERT(!JSScript::isValidOffset(script->globalsOffset));

        nargs = script->bindings.countArgs();
        nvars = script->bindings.countVars();
        argsVars = (nargs << 16) | nvars;
    }
    if (!xdr->codeUint32(&argsVars))
        return false;
    if (mode == XDR_DECODE) {
        nargs = argsVars >> 16;
        nvars = argsVars & 0xFFFF;
    }
    JS_ASSERT(nargs != Bindings::BINDING_COUNT_LIMIT);
    JS_ASSERT(nvars != Bindings::BINDING_COUNT_LIMIT);

    Bindings bindings(cx);
    uint32_t nameCount = nargs + nvars;
    if (nameCount > 0) {
        LifoAllocScope las(&cx->tempLifoAlloc());

        /*
         * To xdr the names we prefix the names with a bitmap descriptor and
         * then xdr the names as strings. For argument names (indexes below
         * nargs) the corresponding bit in the bitmap is unset when the name
         * is null. Such null names are not encoded or decoded. For variable
         * names (indexes starting from nargs) bitmap's bit is set when the
         * name is declared as const, not as ordinary var.
         * */
        unsigned bitmapLength = JS_HOWMANY(nameCount, JS_BITS_PER_UINT32);
        uint32_t *bitmap = cx->tempLifoAlloc().newArray<uint32_t>(bitmapLength);
        if (!bitmap) {
            js_ReportOutOfMemory(cx);
            return false;
        }

        Vector<JSAtom *> names(cx);
        if (mode == XDR_ENCODE) {
            if (!script->bindings.getLocalNameArray(cx, &names))
                return false;
            PodZero(bitmap, bitmapLength);
            for (unsigned i = 0; i < nameCount; i++) {
                if (i < nargs && names[i])
                    bitmap[i >> JS_BITS_PER_UINT32_LOG2] |= JS_BIT(i & (JS_BITS_PER_UINT32 - 1));
            }
        }
        for (unsigned i = 0; i < bitmapLength; ++i) {
            if (!xdr->codeUint32(&bitmap[i]))
                return false;
        }

        for (unsigned i = 0; i < nameCount; i++) {
            if (i < nargs &&
                !(bitmap[i >> JS_BITS_PER_UINT32_LOG2] & JS_BIT(i & (JS_BITS_PER_UINT32 - 1))))
            {
                if (mode == XDR_DECODE) {
                    uint16_t dummy;
                    if (!bindings.addDestructuring(cx, &dummy))
                        return false;
                } else {
                    JS_ASSERT(!names[i]);
                }
                continue;
            }

            JSAtom *name;
            if (mode == XDR_ENCODE)
                name = names[i];
            if (!XDRAtom(xdr, &name))
                return false;
            if (mode == XDR_DECODE) {
                BindingKind kind = (i < nargs)
                                   ? ARGUMENT
                                   : (bitmap[i >> JS_BITS_PER_UINT32_LOG2] &
                                      JS_BIT(i & (JS_BITS_PER_UINT32 - 1))
                                     ? CONSTANT
                                     : VARIABLE);
                if (!bindings.add(cx, name, kind))
                    return false;
            }
        }
    }

    if (mode == XDR_DECODE) {
        if (!bindings.ensureShape(cx))
            return false;
        bindings.makeImmutable();
    }

    if (mode == XDR_ENCODE)
        length = script->length;
    if (!xdr->codeUint32(&length))
        return JS_FALSE;

    if (mode == XDR_ENCODE) {
        prologLength = script->mainOffset;
        JS_ASSERT(script->getVersion() != JSVERSION_UNKNOWN);
        version = (uint32_t)script->getVersion() | (script->nfixed << 16);
        lineno = script->lineno;
        nslots = (uint32_t)script->nslots;
        nslots = (uint32_t)((script->staticLevel << 16) | script->nslots);
        natoms = script->natoms;

        notes = script->notes();
        nsrcnotes = script->numNotes();

        if (JSScript::isValidOffset(script->objectsOffset))
            nobjects = script->objects()->length;
        if (JSScript::isValidOffset(script->regexpsOffset))
            nregexps = script->regexps()->length;
        if (JSScript::isValidOffset(script->trynotesOffset))
            ntrynotes = script->trynotes()->length;
        if (JSScript::isValidOffset(script->constOffset))
            nconsts = script->consts()->length;

        nClosedArgs = script->nClosedArgs;
        nClosedVars = script->nClosedVars;
        encodedClosedCount = (nClosedArgs << 16) | nClosedVars;

        nTypeSets = script->nTypeSets;

        if (script->noScriptRval)
            scriptBits |= (1 << NoScriptRval);
        if (script->savedCallerFun)
            scriptBits |= (1 << SavedCallerFun);
        if (script->strictModeCode)
            scriptBits |= (1 << StrictModeCode);
        if (script->usesEval)
            scriptBits |= (1 << UsesEval);
        if (script->mayNeedArgsObj()) {
            scriptBits |= (1 << MayNeedArgsObj);
            /*
             * In some cases, the front-end calls setNeedsArgsObj when the
             * script definitely needsArgsObj; preserve this information which
             * would otherwise be lost.
             */
            if (script->analyzedArgsUsage() && script->needsArgsObj())
                scriptBits |= (1 << NeedsArgsObj);
        }
        if (script->filename) {
            scriptBits |= (parentScript && parentScript->filename == script->filename)
                          ? (1 << ParentFilename)
                          : (1 << OwnFilename);
        }

        JS_ASSERT(!script->compileAndGo);
        JS_ASSERT(!script->hasSingletons);
    }

    if (!xdr->codeUint32(&prologLength))
        return JS_FALSE;
    if (!xdr->codeUint32(&version))
        return JS_FALSE;

    /*
     * To fuse allocations, we need srcnote, atom, objects, regexp, and trynote
     * counts early.
     */
    if (!xdr->codeUint32(&natoms))
        return JS_FALSE;
    if (!xdr->codeUint32(&nsrcnotes))
        return JS_FALSE;
    if (!xdr->codeUint32(&ntrynotes))
        return JS_FALSE;
    if (!xdr->codeUint32(&nobjects))
        return JS_FALSE;
    if (!xdr->codeUint32(&nregexps))
        return JS_FALSE;
    if (!xdr->codeUint32(&nconsts))
        return JS_FALSE;
    if (!xdr->codeUint32(&encodedClosedCount))
        return JS_FALSE;
    if (!xdr->codeUint32(&nTypeSets))
        return JS_FALSE;
    if (!xdr->codeUint32(&scriptBits))
        return JS_FALSE;

    if (mode == XDR_DECODE) {
        nClosedArgs = encodedClosedCount >> 16;
        nClosedVars = encodedClosedCount & 0xFFFF;

        /* Note: version is packed into the 32b space with another 16b value. */
        JSVersion version_ = JSVersion(version & JS_BITMASK(16));
        JS_ASSERT((version_ & VersionFlags::FULL_MASK) == unsigned(version_));
        script = JSScript::NewScript(cx, length, nsrcnotes, natoms, nobjects,
                                     nregexps, ntrynotes, nconsts, 0, nClosedArgs,
                                     nClosedVars, nTypeSets, version_);
        if (!script)
            return JS_FALSE;

        script->bindings.transfer(cx, &bindings);
        JS_ASSERT(!script->mainOffset);
        script->mainOffset = prologLength;
        script->nfixed = uint16_t(version >> 16);

        /* If we know nsrcnotes, we allocated space for notes in script. */
        notes = script->notes();
        *scriptp = script;

        if (scriptBits & (1 << NoScriptRval))
            script->noScriptRval = true;
        if (scriptBits & (1 << SavedCallerFun))
            script->savedCallerFun = true;
        if (scriptBits & (1 << StrictModeCode))
            script->strictModeCode = true;
        if (scriptBits & (1 << UsesEval))
            script->usesEval = true;
        if (scriptBits & (1 << MayNeedArgsObj)) {
            script->setMayNeedArgsObj();
            if (scriptBits & (1 << NeedsArgsObj))
                script->setNeedsArgsObj(true);
        } else {
            JS_ASSERT(!(scriptBits & (1 << NeedsArgsObj)));
        }
    }

    JS_STATIC_ASSERT(sizeof(jsbytecode) == 1);
    JS_STATIC_ASSERT(sizeof(jssrcnote) == 1);
    if (!xdr->codeBytes(script->code, length) ||
        !xdr->codeBytes(notes, nsrcnotes) ||
        !xdr->codeUint32(&lineno) ||
        !xdr->codeUint32(&nslots)) {
        return false;
    }

    if (scriptBits & (1 << OwnFilename)) {
        const char *filename;
        if (mode == XDR_ENCODE)
            filename = script->filename;
        if (!xdr->codeCString(&filename))
            return false;
        if (mode == XDR_DECODE) {
            script->filename = SaveScriptFilename(cx, filename);
            if (!script->filename)
                return false;
        }
    } else if (scriptBits & (1 << ParentFilename)) {
        JS_ASSERT(parentScript);
        if (mode == XDR_DECODE)
            script->filename = parentScript->filename;
    }

    if (mode == XDR_DECODE) {
        script->lineno = lineno;
        script->nslots = uint16_t(nslots);
        script->staticLevel = uint16_t(nslots >> 16);
        xdr->initScriptPrincipals(script);
    }

    for (i = 0; i != natoms; ++i) {
        if (!XDRAtom(xdr, &script->atoms[i]))
            return false;
    }

    /*
     * Here looping from 0-to-length to xdr objects is essential. It ensures
     * that block objects from the script->objects array will be written and
     * restored in the outer-to-inner order. js_XDRBlockObject relies on this
     * to restore the parent chain.
     */
    for (i = 0; i != nobjects; ++i) {
        HeapPtr<JSObject> *objp = &script->objects()->vector[i];
        uint32_t isBlock;
        if (mode == XDR_ENCODE) {
            JSObject *obj = *objp;
            JS_ASSERT(obj->isFunction() || obj->isStaticBlock());
            isBlock = obj->isBlock() ? 1 : 0;
        }
        if (!xdr->codeUint32(&isBlock))
            return false;
        if (isBlock == 0) {
            JSObject *tmp = *objp;
            if (!XDRInterpretedFunction(xdr, &tmp, parentScript))
                return false;
            *objp = tmp;
        } else {
            JS_ASSERT(isBlock == 1);
            StaticBlockObject *tmp = static_cast<StaticBlockObject *>(objp->get());
            if (!XDRStaticBlockObject(xdr, script, &tmp))
                return false;
            *objp = tmp;
        }
    }
    for (i = 0; i != nregexps; ++i) {
        if (!XDRScriptRegExpObject(xdr, &script->regexps()->vector[i]))
            return false;
    }
    for (i = 0; i != nClosedArgs; ++i) {
        if (!xdr->codeUint32(&script->closedSlots[i]))
            return false;
    }
    for (i = 0; i != nClosedVars; ++i) {
        if (!xdr->codeUint32(&script->closedSlots[nClosedArgs + i]))
            return false;
    }

    if (ntrynotes != 0) {
        /*
         * We combine tn->kind and tn->stackDepth when serializing as XDR is not
         * efficient when serializing small integer types.
         */
        JSTryNote *tn, *tnfirst;
        uint32_t kindAndDepth;
        JS_STATIC_ASSERT(sizeof(tn->kind) == sizeof(uint8_t));
        JS_STATIC_ASSERT(sizeof(tn->stackDepth) == sizeof(uint16_t));

        tnfirst = script->trynotes()->vector;
        JS_ASSERT(script->trynotes()->length == ntrynotes);
        tn = tnfirst + ntrynotes;
        do {
            --tn;
            if (mode == XDR_ENCODE) {
                kindAndDepth = (uint32_t(tn->kind) << 16)
                               | uint32_t(tn->stackDepth);
            }
            if (!xdr->codeUint32(&kindAndDepth) ||
                !xdr->codeUint32(&tn->start) ||
                !xdr->codeUint32(&tn->length)) {
                return false;
            }
            if (mode == XDR_DECODE) {
                tn->kind = uint8_t(kindAndDepth >> 16);
                tn->stackDepth = uint16_t(kindAndDepth);
            }
        } while (tn != tnfirst);
    }

    if (nconsts) {
        HeapValue *vector = script->consts()->vector;
        for (i = 0; i != nconsts; ++i) {
            if (!XDRScriptConst(xdr, &vector[i]))
                return false;
        }
    }

    if (mode == XDR_DECODE) {
        if (cx->hasRunOption(JSOPTION_PCCOUNT))
            (void) script->initCounts(cx);
        *scriptp = script;
    }

    return true;
}

template bool
js::XDRScript(XDRState<XDR_ENCODE> *xdr, JSScript **scriptp, JSScript *parentScript);

template bool
js::XDRScript(XDRState<XDR_DECODE> *xdr, JSScript **scriptp, JSScript *parentScript);

bool
JSScript::initCounts(JSContext *cx)
{
    JS_ASSERT(!pcCounters);

    size_t count = 0;

    jsbytecode *pc, *next;
    for (pc = code; pc < code + length; pc = next) {
        count += OpcodeCounts::numCounts(JSOp(*pc));
        next = pc + GetBytecodeLength(pc);
    }

    size_t bytes = (length * sizeof(OpcodeCounts)) + (count * sizeof(double));
    char *cursor = (char *) cx->calloc_(bytes);
    if (!cursor)
        return false;

    DebugOnly<char *> base = cursor;

    pcCounters.counts = (OpcodeCounts *) cursor;
    cursor += length * sizeof(OpcodeCounts);

    for (pc = code; pc < code + length; pc = next) {
        pcCounters.counts[pc - code].counts = (double *) cursor;
        size_t capacity = OpcodeCounts::numCounts(JSOp(*pc));
#ifdef DEBUG
        pcCounters.counts[pc - code].capacity = capacity;
#endif
        cursor += capacity * sizeof(double);
        next = pc + GetBytecodeLength(pc);
    }

    JS_ASSERT(size_t(cursor - base) == bytes);

    /* Enable interrupts in any interpreter frames running on this script. */
    InterpreterFrames *frames;
    for (frames = cx->runtime->interpreterFrames; frames; frames = frames->older)
        frames->enableInterruptsIfRunning(this);

    return true;
}

void
JSScript::destroyCounts(JSContext *cx)
{
    if (pcCounters) {
        cx->free_(pcCounters.counts);
        pcCounters.counts = NULL;
    }
}

/*
 * Shared script filename management.
 */

const char *
js::SaveScriptFilename(JSContext *cx, const char *filename)
{
    JSCompartment *comp = cx->compartment;

    ScriptFilenameTable::AddPtr p = comp->scriptFilenameTable.lookupForAdd(filename);
    if (!p) {
        size_t size = offsetof(ScriptFilenameEntry, filename) + strlen(filename) + 1;
        ScriptFilenameEntry *entry = (ScriptFilenameEntry *) cx->malloc_(size);
        if (!entry)
            return NULL;
        entry->marked = false;
        strcpy(entry->filename, filename);

        if (!comp->scriptFilenameTable.add(p, entry)) {
            Foreground::free_(entry);
            JS_ReportOutOfMemory(cx);
            return NULL;
        }
    }

    ScriptFilenameEntry *sfe = *p;
#ifdef JSGC_INCREMENTAL
    /*
     * During the IGC we need to ensure that filename is marked whenever it is
     * accessed even if the name was already in the table. At this point old
     * scripts or exceptions pointing to the filename may no longer be
     * reachable.
     */
    if (comp->needsBarrier() && !sfe->marked)
        sfe->marked = true;
#endif

    return sfe->filename;
}

/*
 * Back up from a saved filename by its offset within its hash table entry.
 */
#define FILENAME_TO_SFE(fn) \
    ((ScriptFilenameEntry *) ((fn) - offsetof(ScriptFilenameEntry, filename)))

void
js::MarkScriptFilename(const char *filename)
{
    ScriptFilenameEntry *sfe = FILENAME_TO_SFE(filename);
    sfe->marked = true;
}

void
js::SweepScriptFilenames(JSCompartment *comp)
{
    ScriptFilenameTable &table = comp->scriptFilenameTable;
    for (ScriptFilenameTable::Enum e(table); !e.empty(); e.popFront()) {
        ScriptFilenameEntry *entry = e.front();
        if (entry->marked) {
            entry->marked = false;
        } else if (!comp->rt->gcKeepAtoms) {
            Foreground::free_(entry);
            e.removeFront();
        }
    }
}

void
js::FreeScriptFilenames(JSCompartment *comp)
{
    ScriptFilenameTable &table = comp->scriptFilenameTable;
    for (ScriptFilenameTable::Enum e(table); !e.empty(); e.popFront())
        Foreground::free_(e.front());

    table.clear();
}

/*
 * JSScript data structures memory alignment:
 *
 * JSScript
 * JSObjectArray    script objects' descriptor if JSScript.objectsOffset != 0,
 *                    use script->objects() to access it.
 * JSObjectArray    script regexps' descriptor if JSScript.regexpsOffset != 0,
 *                    use script->regexps() to access it.
 * JSTryNoteArray   script try notes' descriptor if JSScript.tryNotesOffset
 *                    != 0, use script->trynotes() to access it.
 * JSAtom *a[]      array of JSScript.natoms atoms pointed by
 *                    JSScript.atoms if any.
 * JSObject *o[]    array of script->objects()->length objects if any
 *                    pointed by script->objects()->vector.
 * JSObject *r[]    array of script->regexps()->length regexps if any
 *                    pointed by script->regexps()->vector.
 * JSTryNote t[]    array of script->trynotes()->length try notes if any
 *                    pointed by script->trynotes()->vector.
 * jsbytecode b[]   script bytecode pointed by JSScript.code.
 * jssrcnote  s[]   script source notes, use script->notes() to access it
 *
 * The alignment avoids gaps between entries as alignment requirement for each
 * subsequent structure or array is the same or divides the alignment
 * requirement for the previous one.
 *
 * The followings asserts checks that assuming that the alignment requirement
 * for JSObjectArray and JSTryNoteArray are sizeof(void *) and for JSTryNote
 * it is sizeof(uint32_t) as the structure consists of 3 uint32_t fields.
 */
JS_STATIC_ASSERT(sizeof(JSScript) % sizeof(void *) == 0);
JS_STATIC_ASSERT(sizeof(JSObjectArray) % sizeof(void *) == 0);
JS_STATIC_ASSERT(sizeof(JSTryNoteArray) == sizeof(JSObjectArray));
JS_STATIC_ASSERT(sizeof(JSAtom *) == sizeof(JSObject *));
JS_STATIC_ASSERT(sizeof(JSObject *) % sizeof(uint32_t) == 0);
JS_STATIC_ASSERT(sizeof(JSTryNote) == 3 * sizeof(uint32_t));
JS_STATIC_ASSERT(sizeof(uint32_t) % sizeof(jsbytecode) == 0);
JS_STATIC_ASSERT(sizeof(jsbytecode) % sizeof(jssrcnote) == 0);

/*
 * Check that uint8_t offsets is enough to reach any optional array allocated
 * after JSScript. For that we check that the maximum possible offset for
 * JSConstArray, that last optional array, still fits 1 byte and do not
 * coincide with INVALID_OFFSET.
 */
JS_STATIC_ASSERT(sizeof(JSObjectArray) +
                 sizeof(JSObjectArray) +
                 sizeof(JSTryNoteArray) +
                 sizeof(js::GlobalSlotArray)
                 < JSScript::INVALID_OFFSET);
JS_STATIC_ASSERT(JSScript::INVALID_OFFSET <= 255);

JSScript *
JSScript::NewScript(JSContext *cx, uint32_t length, uint32_t nsrcnotes, uint32_t natoms,
                    uint32_t nobjects, uint32_t nregexps,
                    uint32_t ntrynotes, uint32_t nconsts, uint32_t nglobals,
                    uint16_t nClosedArgs, uint16_t nClosedVars, uint32_t nTypeSets, JSVersion version)
{
    size_t size = sizeof(JSAtom *) * natoms;
    if (nobjects != 0)
        size += sizeof(JSObjectArray) + nobjects * sizeof(JSObject *);
    if (nregexps != 0)
        size += sizeof(JSObjectArray) + nregexps * sizeof(JSObject *);
    if (ntrynotes != 0)
        size += sizeof(JSTryNoteArray) + ntrynotes * sizeof(JSTryNote);
    if (nglobals != 0)
        size += sizeof(GlobalSlotArray) + nglobals * sizeof(GlobalSlotArray::Entry);
    uint32_t totalClosed = nClosedArgs + nClosedVars;
    if (totalClosed != 0)
        size += totalClosed * sizeof(uint32_t);

    /*
     * To esnure jsval alignment for the const array we place it immediately
     * after JSSomethingArray structs as their sizes all divide sizeof(jsval).
     * This works as long as the data itself is allocated with proper
     * alignment which we ensure below.
     */
    JS_STATIC_ASSERT(sizeof(JSObjectArray) % sizeof(jsval) == 0);
    JS_STATIC_ASSERT(sizeof(JSTryNoteArray) % sizeof(jsval) == 0);
    JS_STATIC_ASSERT(sizeof(GlobalSlotArray) % sizeof(jsval) == 0);
    JS_STATIC_ASSERT(sizeof(JSConstArray) % sizeof(jsval) == 0);
    if (nconsts != 0)
        size += sizeof(JSConstArray) + nconsts * sizeof(Value);

    size += length * sizeof(jsbytecode) + nsrcnotes * sizeof(jssrcnote);

    /*
     * We assume that calloc aligns on sizeof(Value) if the size we ask to
     * allocate divides sizeof(Value).
     */
    JS_STATIC_ASSERT(sizeof(Value) == sizeof(double));
    uint8_t *data = static_cast<uint8_t *>(cx->calloc_(JS_ROUNDUP(size, sizeof(Value))));
    if (!data)
        return NULL;

    JSScript *script = js_NewGCScript(cx);
    if (!script) {
        Foreground::free_(data);
        return NULL;
    }

    PodZero(script);
#ifdef JS_CRASH_DIAGNOSTICS
    script->cookie1[0] = script->cookie2[0] = JS_SCRIPT_COOKIE;
#endif
    script->data  = data;
    script->length = length;
    script->version = version;
    new (&script->bindings) Bindings(cx);

    uint8_t *cursor = data;
    if (nobjects != 0) {
        script->objectsOffset = uint8_t(cursor - data);
        cursor += sizeof(JSObjectArray);
    } else {
        script->objectsOffset = JSScript::INVALID_OFFSET;
    }
    if (nregexps != 0) {
        script->regexpsOffset = uint8_t(cursor - data);
        cursor += sizeof(JSObjectArray);
    } else {
        script->regexpsOffset = JSScript::INVALID_OFFSET;
    }
    if (ntrynotes != 0) {
        script->trynotesOffset = uint8_t(cursor - data);
        cursor += sizeof(JSTryNoteArray);
    } else {
        script->trynotesOffset = JSScript::INVALID_OFFSET;
    }
    if (nglobals != 0) {
        script->globalsOffset = uint8_t(cursor - data);
        cursor += sizeof(GlobalSlotArray);
    } else {
        script->globalsOffset = JSScript::INVALID_OFFSET;
    }
    JS_ASSERT(cursor - data < 0xFF);
    if (nconsts != 0) {
        script->constOffset = uint8_t(cursor - data);
        cursor += sizeof(JSConstArray);
    } else {
        script->constOffset = JSScript::INVALID_OFFSET;
    }

    JS_STATIC_ASSERT(sizeof(JSObjectArray) +
                     sizeof(JSObjectArray) +
                     sizeof(JSTryNoteArray) +
                     sizeof(GlobalSlotArray) < 0xFF);


    if (nconsts != 0) {
        JS_ASSERT(reinterpret_cast<uintptr_t>(cursor) % sizeof(jsval) == 0);
        script->consts()->length = nconsts;
        script->consts()->vector = (HeapValue *)cursor;
        cursor += nconsts * sizeof(script->consts()->vector[0]);
    }

    if (natoms != 0) {
        script->natoms = natoms;
        script->atoms = reinterpret_cast<JSAtom **>(cursor);
        cursor += natoms * sizeof(script->atoms[0]);
    }

    if (nobjects != 0) {
        script->objects()->length = nobjects;
        script->objects()->vector = (HeapPtr<JSObject> *)cursor;
        cursor += nobjects * sizeof(script->objects()->vector[0]);
    }

    if (nregexps != 0) {
        script->regexps()->length = nregexps;
        script->regexps()->vector = (HeapPtr<JSObject> *)cursor;
        cursor += nregexps * sizeof(script->regexps()->vector[0]);
    }

    if (ntrynotes != 0) {
        script->trynotes()->length = ntrynotes;
        script->trynotes()->vector = reinterpret_cast<JSTryNote *>(cursor);
        size_t vectorSize = ntrynotes * sizeof(script->trynotes()->vector[0]);
#ifdef DEBUG
        memset(cursor, 0, vectorSize);
#endif
        cursor += vectorSize;
    }

    if (nglobals != 0) {
        script->globals()->length = nglobals;
        script->globals()->vector = reinterpret_cast<GlobalSlotArray::Entry *>(cursor);
        cursor += nglobals * sizeof(script->globals()->vector[0]);
    }

    if (totalClosed != 0) {
        script->nClosedArgs = nClosedArgs;
        script->nClosedVars = nClosedVars;
        script->closedSlots = reinterpret_cast<uint32_t *>(cursor);
        cursor += totalClosed * sizeof(uint32_t);
    }

    JS_ASSERT(nTypeSets <= UINT16_MAX);
    script->nTypeSets = uint16_t(nTypeSets);

    script->code = (jsbytecode *)cursor;
    JS_ASSERT(cursor + length * sizeof(jsbytecode) + nsrcnotes * sizeof(jssrcnote) == data + size);

#ifdef DEBUG
    script->id_ = 0;
#endif

    JS_ASSERT(script->getVersion() == version);
    return script;
}

JSScript *
JSScript::NewScriptFromEmitter(JSContext *cx, BytecodeEmitter *bce)
{
    uint32_t mainLength, prologLength, nfixed;
    JSScript *script;
    const char *filename;
    JSFunction *fun;

    /* The counts of indexed things must be checked during code generation. */
    JS_ASSERT(bce->atomIndices->count() <= INDEX_LIMIT);
    JS_ASSERT(bce->objectList.length <= INDEX_LIMIT);
    JS_ASSERT(bce->regexpList.length <= INDEX_LIMIT);

    mainLength = bce->offset();
    prologLength = bce->prologOffset();

    if (!bce->bindings.ensureShape(cx))
        return NULL;

    uint32_t nsrcnotes = uint32_t(bce->countFinalSourceNotes());
    uint16_t nClosedArgs = uint16_t(bce->closedArgs.length());
    JS_ASSERT(nClosedArgs == bce->closedArgs.length());
    uint16_t nClosedVars = uint16_t(bce->closedVars.length());
    JS_ASSERT(nClosedVars == bce->closedVars.length());
    script = NewScript(cx, prologLength + mainLength, nsrcnotes,
                       bce->atomIndices->count(), bce->objectList.length,
                       bce->regexpList.length, bce->ntrynotes, bce->constList.length(),
                       bce->globalUses.length(), nClosedArgs, nClosedVars,
                       bce->typesetCount, bce->version());
    if (!script)
        return NULL;

    bce->bindings.makeImmutable();

    JS_ASSERT(script->mainOffset == 0);
    script->mainOffset = prologLength;
    PodCopy<jsbytecode>(script->code, bce->prologBase(), prologLength);
    PodCopy<jsbytecode>(script->main(), bce->base(), mainLength);
    nfixed = bce->inFunction() ? bce->bindings.countVars() : 0;
    JS_ASSERT(nfixed < SLOTNO_LIMIT);
    script->nfixed = uint16_t(nfixed);
    js_InitAtomMap(cx, bce->atomIndices.getMap(), script->atoms);

    filename = bce->parser->tokenStream.getFilename();
    if (filename) {
        script->filename = SaveScriptFilename(cx, filename);
        if (!script->filename)
            return NULL;
    }
    script->lineno = bce->firstLine;
    if (script->nfixed + bce->maxStackDepth >= JS_BIT(16)) {
        ReportCompileErrorNumber(cx, bce->tokenStream(), NULL, JSREPORT_ERROR, JSMSG_NEED_DIET,
                                 "script");
        return NULL;
    }
    script->nslots = script->nfixed + bce->maxStackDepth;
    script->staticLevel = uint16_t(bce->staticLevel);
    script->principals = bce->parser->principals;

    if (script->principals)
        JS_HoldPrincipals(script->principals);

    /* Establish invariant: principals implies originPrincipals. */
    script->originPrincipals = bce->parser->originPrincipals;
    if (!script->originPrincipals)
        script->originPrincipals = script->principals;
    if (script->originPrincipals)
        JS_HoldPrincipals(script->originPrincipals);

    script->sourceMap = (jschar *) bce->parser->tokenStream.releaseSourceMap();

    if (!FinishTakingSrcNotes(cx, bce, script->notes()))
        return NULL;
    if (bce->ntrynotes != 0)
        FinishTakingTryNotes(bce, script->trynotes());
    if (bce->objectList.length != 0)
        bce->objectList.finish(script->objects());
    if (bce->regexpList.length != 0)
        bce->regexpList.finish(script->regexps());
    if (bce->constList.length() != 0)
        bce->constList.finish(script->consts());
    if (bce->flags & TCF_NO_SCRIPT_RVAL)
        script->noScriptRval = true;
    if (bce->flags & TCF_STRICT_MODE_CODE)
        script->strictModeCode = true;
    if (bce->flags & TCF_COMPILE_N_GO) {
        script->compileAndGo = true;
        const StackFrame *fp = bce->parser->callerFrame;
        if (fp && fp->isFunctionFrame())
            script->savedCallerFun = true;
    }
    if (bce->callsEval())
        script->usesEval = true;
    if (bce->flags & TCF_HAS_SINGLETONS)
        script->hasSingletons = true;

    /*
     * The arguments-usage analysis in analyzeSSA only looks at
     * JSOP_ARGUMENTS use. Therefore, anything else that definitely requires an
     * arguments object needs to be accounted for here.
     */
    if (bce->inFunction()) {
        bool needsArgsObj = bce->mayOverwriteArguments() || bce->needsEagerArguments();
        if (needsArgsObj || bce->usesArguments()) {
            script->setMayNeedArgsObj();
            if (needsArgsObj)
                script->setNeedsArgsObj(true);
        }
    }

    if (bce->globalUses.length()) {
        PodCopy<GlobalSlotArray::Entry>(script->globals()->vector, &bce->globalUses[0],
                                        bce->globalUses.length());
    }

    if (script->nClosedArgs)
        PodCopy<uint32_t>(script->closedSlots, &bce->closedArgs[0], script->nClosedArgs);
    if (script->nClosedVars) {
        PodCopy<uint32_t>(&script->closedSlots[script->nClosedArgs], &bce->closedVars[0],
                          script->nClosedVars);
    }

    script->bindings.transfer(cx, &bce->bindings);

    fun = NULL;
    if (bce->inFunction()) {
        /*
         * We initialize fun->script() to be the script constructed above
         * so that the debugger has a valid fun->script().
         */
        fun = bce->fun();
        JS_ASSERT(fun->isInterpreted());
        JS_ASSERT(!fun->script());
        if (bce->flags & TCF_FUN_HEAVYWEIGHT)
            fun->flags |= JSFUN_HEAVYWEIGHT;

        /*
         * Mark functions which will only be executed once as singletons.
         */
        bool singleton =
            cx->typeInferenceEnabled() &&
            bce->parent &&
            bce->parent->compiling() &&
            bce->parent->asBytecodeEmitter()->checkSingletonContext();

        if (!script->typeSetFunction(cx, fun, singleton))
            return NULL;

        fun->setScript(script);
        script->globalObject = fun->getParent() ? &fun->getParent()->global() : NULL;
    } else {
        /*
         * Initialize script->object, if necessary, so that the debugger has a
         * valid holder object.
         */
        if (bce->flags & TCF_NEED_SCRIPT_GLOBAL)
            script->globalObject = GetCurrentGlobal(cx);
    }

    /* Tell the debugger about this compiled script. */
    js_CallNewScriptHook(cx, script, fun);
    if (!bce->parent) {
        GlobalObject *compileAndGoGlobal = NULL;
        if (script->compileAndGo) {
            compileAndGoGlobal = script->globalObject;
            if (!compileAndGoGlobal)
                compileAndGoGlobal = &bce->scopeChain()->global();
        }
        Debugger::onNewScript(cx, script, compileAndGoGlobal);
    }

    if (cx->hasRunOption(JSOPTION_PCCOUNT))
        (void) script->initCounts(cx);

    return script;
}

size_t
JSScript::computedSizeOfData()
{
    uint8_t *dataEnd = code + length * sizeof(jsbytecode) + numNotes() * sizeof(jssrcnote);
    JS_ASSERT(dataEnd >= data);
    return dataEnd - data;
}

size_t
JSScript::sizeOfData(JSMallocSizeOfFun mallocSizeOf)
{
    return mallocSizeOf(data);
}

/*
 * Nb: srcnotes are variable-length.  This function computes the number of
 * srcnote *slots*, which may be greater than the number of srcnotes.
 */
uint32_t
JSScript::numNotes()
{
    jssrcnote *sn;
    jssrcnote *notes_ = notes();
    for (sn = notes_; !SN_IS_TERMINATOR(sn); sn = SN_NEXT(sn))
        continue;
    return sn - notes_ + 1;    /* +1 for the terminator */
}

JS_FRIEND_API(void)
js_CallNewScriptHook(JSContext *cx, JSScript *script, JSFunction *fun)
{
    JS_ASSERT(!script->callDestroyHook);
    if (JSNewScriptHook hook = cx->runtime->debugHooks.newScriptHook) {
        AutoKeepAtoms keep(cx->runtime);
        hook(cx, script->filename, script->lineno, script, fun,
             cx->runtime->debugHooks.newScriptHookData);
    }
    script->callDestroyHook = true;
}

void
js_CallDestroyScriptHook(JSContext *cx, JSScript *script)
{
    if (!script->callDestroyHook)
        return;

    if (JSDestroyScriptHook hook = cx->runtime->debugHooks.destroyScriptHook)
        hook(cx, script, cx->runtime->debugHooks.destroyScriptHookData);
    script->callDestroyHook = false;
    JS_ClearScriptTraps(cx, script);
}

#ifdef JS_CRASH_DIAGNOSTICS

void
JSScript::CheckScript(JSScript *prev)
{
    if (cookie1[0] != JS_SCRIPT_COOKIE || cookie2[0] != JS_SCRIPT_COOKIE) {
        crash::StackBuffer<sizeof(JSScript), 0x87> buf1(this);
        crash::StackBuffer<sizeof(JSScript), 0x88> buf2(prev);
        JS_OPT_ASSERT(false);
    }
}

#endif /* JS_CRASH_DIAGNOSTICS */

void
JSScript::finalize(JSContext *cx, bool background)
{
    CheckScript(NULL);

    js_CallDestroyScriptHook(cx, this);

    JS_ASSERT_IF(principals, originPrincipals);
    if (principals)
        JS_DropPrincipals(cx->runtime, principals);
    if (originPrincipals)
        JS_DropPrincipals(cx->runtime, originPrincipals);

    if (types)
        types->destroy();

#ifdef JS_METHODJIT
    mjit::ReleaseScriptCode(cx, this);
#endif

    destroyCounts(cx);

    if (sourceMap)
        cx->free_(sourceMap);

    if (debug) {
        jsbytecode *end = code + length;
        for (jsbytecode *pc = code; pc < end; pc++) {
            if (BreakpointSite *site = getBreakpointSite(pc)) {
                /* Breakpoints are swept before finalization. */
                JS_ASSERT(site->firstBreakpoint() == NULL);
                site->clearTrap(cx, NULL, NULL);
                JS_ASSERT(getBreakpointSite(pc) == NULL);
            }
        }
        cx->free_(debug);
    }

    JS_POISON(data, 0xdb, computedSizeOfData());
    cx->free_(data);
}

namespace js {

static const uint32_t GSN_CACHE_THRESHOLD = 100;
static const uint32_t GSN_CACHE_MAP_INIT_SIZE = 20;

void
GSNCache::purge()
{
    code = NULL;
    if (map.initialized())
        map.finish();
}

} /* namespace js */

jssrcnote *
js_GetSrcNoteCached(JSContext *cx, JSScript *script, jsbytecode *pc)
{
    size_t target = pc - script->code;
    if (target >= size_t(script->length))
        return NULL;

    GSNCache *cache = GetGSNCache(cx);
    if (cache->code == script->code) {
        JS_ASSERT(cache->map.initialized());
        GSNCache::Map::Ptr p = cache->map.lookup(pc);
        return p ? p->value : NULL;
    }

    size_t offset = 0;
    jssrcnote *result;
    for (jssrcnote *sn = script->notes(); ; sn = SN_NEXT(sn)) {
        if (SN_IS_TERMINATOR(sn)) {
            result = NULL;
            break;
        }
        offset += SN_DELTA(sn);
        if (offset == target && SN_IS_GETTABLE(sn)) {
            result = sn;
            break;
        }
    }

    if (cache->code != script->code && script->length >= GSN_CACHE_THRESHOLD) {
        unsigned nsrcnotes = 0;
        for (jssrcnote *sn = script->notes(); !SN_IS_TERMINATOR(sn);
             sn = SN_NEXT(sn)) {
            if (SN_IS_GETTABLE(sn))
                ++nsrcnotes;
        }
        if (cache->code) {
            JS_ASSERT(cache->map.initialized());
            cache->map.finish();
            cache->code = NULL;
        }
        if (cache->map.init(nsrcnotes)) {
            pc = script->code;
            for (jssrcnote *sn = script->notes(); !SN_IS_TERMINATOR(sn);
                 sn = SN_NEXT(sn)) {
                pc += SN_DELTA(sn);
                if (SN_IS_GETTABLE(sn))
                    JS_ALWAYS_TRUE(cache->map.put(pc, sn));
            }
            cache->code = script->code;
        }
    }

    return result;
}

unsigned
js::PCToLineNumber(unsigned startLine, jssrcnote *notes, jsbytecode *code, jsbytecode *pc)
{
    unsigned lineno = startLine;

    /*
     * Walk through source notes accumulating their deltas, keeping track of
     * line-number notes, until we pass the note for pc's offset within
     * script->code.
     */
    ptrdiff_t offset = 0;
    ptrdiff_t target = pc - code;
    for (jssrcnote *sn = notes; !SN_IS_TERMINATOR(sn); sn = SN_NEXT(sn)) {
        offset += SN_DELTA(sn);
        SrcNoteType type = (SrcNoteType) SN_TYPE(sn);
        if (type == SRC_SETLINE) {
            if (offset <= target)
                lineno = (unsigned) js_GetSrcNoteOffset(sn, 0);
        } else if (type == SRC_NEWLINE) {
            if (offset <= target)
                lineno++;
        }
        if (offset > target)
            break;
    }

    return lineno;
}

unsigned
js::PCToLineNumber(JSScript *script, jsbytecode *pc)
{
    /* Cope with StackFrame.pc value prior to entering js_Interpret. */
    if (!pc)
        return 0;

    return PCToLineNumber(script->lineno, script->notes(), script->code, pc);
}

/* The line number limit is the same as the jssrcnote offset limit. */
#define SN_LINE_LIMIT   (SN_3BYTE_OFFSET_FLAG << 16)

jsbytecode *
js_LineNumberToPC(JSScript *script, unsigned target)
{
    ptrdiff_t offset = 0;
    ptrdiff_t best = -1;
    unsigned lineno = script->lineno;
    unsigned bestdiff = SN_LINE_LIMIT;
    for (jssrcnote *sn = script->notes(); !SN_IS_TERMINATOR(sn); sn = SN_NEXT(sn)) {
        /*
         * Exact-match only if offset is not in the prolog; otherwise use
         * nearest greater-or-equal line number match.
         */
        if (lineno == target && offset >= ptrdiff_t(script->mainOffset))
            goto out;
        if (lineno >= target) {
            unsigned diff = lineno - target;
            if (diff < bestdiff) {
                bestdiff = diff;
                best = offset;
            }
        }
        offset += SN_DELTA(sn);
        SrcNoteType type = (SrcNoteType) SN_TYPE(sn);
        if (type == SRC_SETLINE) {
            lineno = (unsigned) js_GetSrcNoteOffset(sn, 0);
        } else if (type == SRC_NEWLINE) {
            lineno++;
        }
    }
    if (best >= 0)
        offset = best;
out:
    return script->code + offset;
}

JS_FRIEND_API(unsigned)
js_GetScriptLineExtent(JSScript *script)
{
    unsigned lineno = script->lineno;
    unsigned maxLineNo = 0;
    bool counting = true;
    for (jssrcnote *sn = script->notes(); !SN_IS_TERMINATOR(sn); sn = SN_NEXT(sn)) {
        SrcNoteType type = (SrcNoteType) SN_TYPE(sn);
        if (type == SRC_SETLINE) {
            if (maxLineNo < lineno)
                maxLineNo = lineno;
            lineno = (unsigned) js_GetSrcNoteOffset(sn, 0);
            counting = true;
            if (maxLineNo < lineno)
                maxLineNo = lineno;
            else
                counting = false;
        } else if (type == SRC_NEWLINE) {
            if (counting)
                lineno++;
        }
    }

    if (maxLineNo > lineno)
        lineno = maxLineNo;

    return 1 + lineno - script->lineno;
}

namespace js {

unsigned
CurrentLine(JSContext *cx)
{
    return PCToLineNumber(cx->fp()->script(), cx->regs().pc);
}

void
CurrentScriptFileLineOriginSlow(JSContext *cx, const char **file, unsigned *linenop,
                                JSPrincipals **origin)
{
    FrameRegsIter iter(cx);
    while (!iter.done() && !iter.fp()->isScriptFrame())
        ++iter;

    if (iter.done()) {
        *file = NULL;
        *linenop = 0;
        *origin = NULL;
        return;
    }

    JSScript *script = iter.fp()->script();
    *file = script->filename;
    *linenop = PCToLineNumber(iter.fp()->script(), iter.pc());
    *origin = script->originPrincipals;
}

}  /* namespace js */

JSScript *
js::CloneScript(JSContext *cx, JSScript *script)
{
    JS_ASSERT(cx->compartment != script->compartment());

    /* Serialize script. */
    XDREncoder encoder(cx);

    if (!XDRScript(&encoder, &script, NULL))
        return NULL;

    uint32_t nbytes;
    const void *p = encoder.getData(&nbytes);

    /* De-serialize script. */
    XDRDecoder decoder(cx, p, nbytes, cx->compartment->principals, script->originPrincipals);

    JSScript *newScript;
    if (!XDRScript(&decoder, &newScript, NULL))
        return NULL;

    return newScript;
}

void
JSScript::copyClosedSlotsTo(JSScript *other)
{
    js_memcpy(other->closedSlots, closedSlots, nClosedArgs + nClosedVars);
}

bool
JSScript::ensureHasDebug(JSContext *cx)
{
    if (debug)
        return true;

    size_t nbytes = offsetof(DebugScript, breakpoints) + length * sizeof(BreakpointSite*);
    debug = (DebugScript *) cx->calloc_(nbytes);
    if (!debug)
        return false;

    /*
     * Ensure that any Interpret() instances running on this script have
     * interrupts enabled. The interrupts must stay enabled until the
     * debug state is destroyed.
     */
    InterpreterFrames *frames;
    for (frames = cx->runtime->interpreterFrames; frames; frames = frames->older)
        frames->enableInterruptsIfRunning(this);

    return true;
}

bool
JSScript::recompileForStepMode(JSContext *cx)
{
#ifdef JS_METHODJIT
    if (jitNormal || jitCtor) {
        mjit::Recompiler::clearStackReferences(cx, this);
        mjit::ReleaseScriptCode(cx, this);
    }
#endif
    return true;
}

bool
JSScript::tryNewStepMode(JSContext *cx, uint32_t newValue)
{
    JS_ASSERT(debug);

    uint32_t prior = debug->stepMode;
    debug->stepMode = newValue;

    if (!prior != !newValue) {
        /* Step mode has been enabled or disabled. Alert the methodjit. */
        if (!recompileForStepMode(cx)) {
            debug->stepMode = prior;
            return false;
        }

        if (!stepModeEnabled() && !debug->numSites) {
            cx->free_(debug);
            debug = NULL;
        }
    }

    return true;
}

bool
JSScript::setStepModeFlag(JSContext *cx, bool step)
{
    if (!ensureHasDebug(cx))
        return false;

    return tryNewStepMode(cx, (debug->stepMode & stepCountMask) | (step ? stepFlagMask : 0));
}

bool
JSScript::changeStepModeCount(JSContext *cx, int delta)
{
    if (!ensureHasDebug(cx))
        return false;

    assertSameCompartment(cx, this);
    JS_ASSERT_IF(delta > 0, cx->compartment->debugMode());

    uint32_t count = debug->stepMode & stepCountMask;
    JS_ASSERT(((count + delta) & stepCountMask) == count + delta);
    return tryNewStepMode(cx,
                          (debug->stepMode & stepFlagMask) |
                          ((count + delta) & stepCountMask));
}

BreakpointSite *
JSScript::getOrCreateBreakpointSite(JSContext *cx, jsbytecode *pc,
                                    GlobalObject *scriptGlobal)
{
    JS_ASSERT(size_t(pc - code) < length);

    if (!ensureHasDebug(cx))
        return NULL;

    BreakpointSite *&site = debug->breakpoints[pc - code];

    if (!site) {
        site = cx->runtime->new_<BreakpointSite>(this, pc);
        if (!site) {
            js_ReportOutOfMemory(cx);
            return NULL;
        }
        debug->numSites++;
    }

    if (site->scriptGlobal)
        JS_ASSERT_IF(scriptGlobal, site->scriptGlobal == scriptGlobal);
    else
        site->scriptGlobal = scriptGlobal;

    return site;
}

void
JSScript::destroyBreakpointSite(JSRuntime *rt, jsbytecode *pc)
{
    JS_ASSERT(unsigned(pc - code) < length);

    BreakpointSite *&site = debug->breakpoints[pc - code];
    JS_ASSERT(site);

    rt->delete_(site);
    site = NULL;

    if (--debug->numSites == 0 && !stepModeEnabled()) {
        rt->free_(debug);
        debug = NULL;
    }
}

void
JSScript::clearBreakpointsIn(JSContext *cx, js::Debugger *dbg, JSObject *handler)
{
    if (!hasAnyBreakpointsOrStepMode())
        return;

    jsbytecode *end = code + length;
    for (jsbytecode *pc = code; pc < end; pc++) {
        BreakpointSite *site = getBreakpointSite(pc);
        if (site) {
            Breakpoint *nextbp;
            for (Breakpoint *bp = site->firstBreakpoint(); bp; bp = nextbp) {
                nextbp = bp->nextInSite();
                if ((!dbg || bp->debugger == dbg) && (!handler || bp->getHandler() == handler))
                    bp->destroy(cx);
            }
        }
    }
}

void
JSScript::clearTraps(JSContext *cx)
{
    if (!hasAnyBreakpointsOrStepMode())
        return;

    jsbytecode *end = code + length;
    for (jsbytecode *pc = code; pc < end; pc++) {
        BreakpointSite *site = getBreakpointSite(pc);
        if (site)
            site->clearTrap(cx);
    }
}

void
JSScript::markChildren(JSTracer *trc)
{
    CheckScript(NULL);

    JS_ASSERT_IF(trc->runtime->gcCheckCompartment,
                 compartment() == trc->runtime->gcCheckCompartment);

    for (uint32_t i = 0; i < natoms; ++i) {
        if (atoms[i])
            MarkStringUnbarriered(trc, &atoms[i], "atom");
    }

    if (JSScript::isValidOffset(objectsOffset)) {
        JSObjectArray *objarray = objects();
        MarkObjectRange(trc, objarray->length, objarray->vector, "objects");
    }

    if (JSScript::isValidOffset(regexpsOffset)) {
        JSObjectArray *objarray = regexps();
        MarkObjectRange(trc, objarray->length, objarray->vector, "objects");
    }

    if (JSScript::isValidOffset(constOffset)) {
        JSConstArray *constarray = consts();
        MarkValueRange(trc, constarray->length, constarray->vector, "consts");
    }

    if (function())
        MarkObject(trc, &function_, "function");

    if (!isCachedEval && globalObject)
        MarkObject(trc, &globalObject, "object");

    if (IS_GC_MARKING_TRACER(trc) && filename)
        MarkScriptFilename(filename);

    bindings.trace(trc);

    if (types)
        types->trace(trc);

    if (hasAnyBreakpointsOrStepMode()) {
        for (unsigned i = 0; i < length; i++) {
            BreakpointSite *site = debug->breakpoints[i];
            if (site && site->trapHandler)
                MarkValue(trc, &site->trapClosure, "trap closure");
        }
    }
}

void
JSScript::setNeedsArgsObj(bool needsArgsObj)
{
    JS_ASSERT(!analyzedArgsUsage_);
    analyzedArgsUsage_ = true;
    needsArgsObj_ = needsArgsObj;
}

bool
JSScript::applySpeculationFailed(JSContext *cx)
{
    JS_ASSERT(analyzedArgsUsage());
    JS_ASSERT(!needsArgsObj());
    needsArgsObj_ = true;

    /*
     * By design, the apply-arguments optimization is only made when there
     * are no outstanding cases of MagicValue(JS_OPTIMIZED_ARGUMENTS) other
     * than this particular invocation of 'f.apply(x, arguments)'. Thus, there
     * are no outstanding values of MagicValue(JS_OPTIMIZED_ARGUMENTS) on the
     * stack. However, there are three things that need fixup:
     *  - there may be any number of activations of this script that don't have
     *    an argsObj that now need one.
     *  - jit code compiled (and possible active on the stack) with the static
     *    assumption of !script->needsArgsObj();
     *  - type inference data for the script assuming script->needsArgsObj; and
     */
    for (AllFramesIter i(cx->stack.space()); !i.done(); ++i) {
        StackFrame *fp = i.fp();
        if (fp->isFunctionFrame() && fp->script() == this) {
            if (!fp->hasArgsObj() && !ArgumentsObject::create(cx, fp)) {
                /*
                 * We can't leave stack frames where fp->script->needsArgsObj
                 * and !fp->hasArgsObj. It is, however, safe to leave frames
                 * where fp->hasArgsObj and !fp->script->needsArgsObj.
                 */
                needsArgsObj_ = false;
                return false;
            }
        }
    }

#ifdef JS_METHODJIT
    if (hasJITCode()) {
        mjit::Recompiler::clearStackReferences(cx, this);
        mjit::ReleaseScriptCode(cx, this);
    }
#endif

    if (hasAnalysis() && analysis()->ranInference()) {
        types::AutoEnterTypeInference enter(cx);
        for (unsigned off = 0; off < length; off += GetBytecodeLength(code + off)) {
            if (code[off] == JSOP_ARGUMENTS) {
                types::TypeSet *set = analysis()->pushedTypes(off, 0);
                JS_ASSERT(set->isLazyArguments(cx));
                set->addType(cx, types::Type::UnknownType());
            }
        }
    }

    return true;
}
