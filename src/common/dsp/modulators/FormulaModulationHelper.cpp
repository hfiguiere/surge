/*
** Surge Synthesizer is Free and Open Source Software
**
** Surge is made available under the Gnu General Public License, v3.0
** https://www.gnu.org/licenses/gpl-3.0.en.html
**
** Copyright 2004-2021 by various individuals as described by the Git transaction log
**
** All source at: https://github.com/surge-synthesizer/surge.git
**
** Surge was a commercial product from 2004-2018, with Copyright and ownership
** in that period held by Claes Johanson at Vember Audio. Claes made Surge
** open source in September 2018.
*/

#include "FormulaModulationHelper.h"
#include <thread>
#include <functional>

namespace Surge
{
namespace Formula
{

// Stack guard leak debugger
struct SGLD
{
    SGLD(const std::string &lab, lua_State *L) : label(lab), L(L) { top = lua_gettop(L); }

    ~SGLD()
    {
        auto nt = lua_gettop(L);
        if (nt != top)
        {
            std::cout << "Guarded stack leak: [" << label << "] exit=" << nt << " enter=" << top
                      << std::endl;
            if (label == "valueAt")
                std::cout << "Q" << std::endl;
        }
    }

    std::string label;
    lua_State *L;
    int top;
};

bool prepareForEvaluation(FormulaModulatorStorage *fs, EvaluatorState &s, bool is_display)
{
    static lua_State *audioState = nullptr, *displayState = nullptr;

    if (!is_display)
    {
        static int aid = 1;
        if (audioState == nullptr)
        {
            audioState = lua_open();
            luaL_openlibs(audioState);
        }
        s.L = audioState;
        snprintf(s.stateName, TXT_SIZE, "audiostate_%d", aid);
        aid++;
        if (aid < 0)
            aid = 1;
    }
    else
    {
        static int did = 1;
        if (displayState == nullptr)
        {
            displayState = lua_open();
            luaL_openlibs(displayState);
        }
        s.L = displayState;
        snprintf(s.stateName, TXT_SIZE, "dispstate_%d", did);
        did++;
        if (did < 0)
            did = 1;
    }

    auto lg = SGLD("prepareForEvaluation", s.L);
    // OK so now evaluate the formula. This is a mistake - the loading and
    // compiling can be expensive so lets look it up by hash first
    auto h = fs->formulaHash;
    auto pvn = std::string("pvn") + std::to_string(is_display) + "_" + std::to_string(h);
    auto pvf = pvn + "_f";
    snprintf(s.funcName, TXT_SIZE, "%s", pvf.c_str());

    // Handle hash collisions
    lua_getglobal(s.L, pvn.c_str());
    s.isvalid = false;

    bool hasString = false;
    if (lua_isstring(s.L, -1))
    {
        if (fs->formulaString != lua_tostring(s.L, -1))
        {
            s.adderror("Hash Collision in function. Bad luck!");
        }
        else
        {
            hasString = true;
        }
    }
    lua_pop(s.L, 1); // we don't need the string or whatever on the stack
    if (hasString)
    {
        snprintf(s.funcName, TXT_SIZE, "%s", pvf.c_str());
        s.isvalid = true;
    }
    else
    {
        const char *lua_script = fs->formulaString.c_str();
        int load_stat = luaL_loadbuffer(s.L, lua_script, strlen(lua_script), lua_script);
        auto res = lua_pcall(s.L, 0, 0, 0); // FIXME error

        if (res == LUA_OK)
        {
            // great now get the modfunc
            lua_getglobal(s.L, "process");
            if (lua_isfunction(s.L, -1))
            {
                // Great - rename it
                lua_setglobal(s.L, s.funcName);
                lua_pushnil(s.L);
                lua_setglobal(s.L, "process");

                lua_getglobal(s.L, s.funcName);
                lua_createtable(s.L, 0, 10);
                // stack is now func > table

                lua_pushstring(s.L, "math");
                lua_getglobal(s.L, "math");
                // stack is now func > table > "math" > (math)

                lua_settable(s.L, -3);

                // stack is now func > table again *BUT* now load math in stripped
                lua_getglobal(s.L, "math");
                lua_pushnil(s.L);

                // func > table > (math) > nil so lua next -2 will iterate over (math)
                while (lua_next(s.L, -2))
                {
                    // stack is now f>t>(m)>k>v
                    lua_pushvalue(s.L, -2);
                    lua_pushvalue(s.L, -2);
                    // stack is now f>t>(m)>k>v>k>v and we want k>v in the table
                    lua_settable(s.L, -6);
                    // stack is now f>t>(m)>k>v and we want the key on top for next so
                    lua_pop(s.L, 1);
                }

                lua_pop(s.L, 1);
                // and now we are back to f>t so we can setfenv it
                lua_setfenv(s.L, -2);

                // and so we are now back to f which we no longer need so
                lua_pop(s.L, 1);
                s.isvalid = true;
            }
            else
            {
                // FIXME error
                s.adderror("After parsing formula, no function 'process' present. You must define "
                           "a function called 'process' in your LUA.");
                // Pop whatever I got
                lua_pop(s.L, 1);
                s.isvalid = false;
            }
        }
        else
        {
            std::ostringstream oss;
            oss << "LUA Raised an error parsing formula: " << lua_tostring(s.L, -1);
            s.adderror(oss.str());
        }

        // this happens here because we did parse it at least. Don't parse again until it is changed
        lua_pushstring(s.L, lua_script);
        lua_setglobal(s.L, pvn.c_str());
    }

    if (s.isvalid)
    {
        // Create my state object each time
        lua_createtable(s.L, 0, 10);
        lua_setglobal(s.L,
                      s.stateName); // FIXME - we have to clean this up when evaluat is done
    }

    if (is_display)
    {
        auto dg = SGLD("set RNG", s.L);
        // Seed the RNG
        lua_getglobal(s.L, "math");
        // > math
        if (lua_isnil(s.L, -1))
        {
            std::cout << "NIL MATH " << std::endl;
        }
        else
        {
            lua_pushstring(s.L, "randomseed");
            lua_gettable(s.L, -2);
            // > math > randomseed
            if (lua_isnil(s.L, -1))
            {
                std::cout << "NUL randomseed" << std::endl;
                lua_pop(s.L, -1);
            }
            else
            {
                lua_pushnumber(s.L, 8675309);
                lua_pcall(s.L, 1, 0, 0);
            }
        }
        // math or nil so
        lua_pop(s.L, 1);
    }

    s.useEnvelope = true;

    s.del = 0;
    s.dec = 0;
    s.a = 0;
    s.h = 0;
    s.r = 0;
    s.s = 0;
    s.rate = 0;
    s.phase = 0;
    s.amp = 0;
    s.deform = 0;
    s.tempo = 120;

    if (s.raisedError)
        std::cout << "ERROR: " << s.error << std::endl;
    return true;
}

bool cleanEvaluatorState(EvaluatorState &s)
{
    if (s.L && s.stateName[0] != 0)
    {
        lua_pushnil(s.L);
        lua_setglobal(s.L, s.stateName);
        s.stateName[0] = 0;
    }
    return true;
}

bool initEvaluatorState(EvaluatorState &s)
{
    s.funcName[0] = 0;
    s.stateName[0] = 0;
    s.L = nullptr;
    return true;
}
float valueAt(int phaseIntPart, float phaseFracPart, FormulaModulatorStorage *fs, EvaluatorState *s)
{
    if (s->L == nullptr)
        return 0;

    if (!s->isvalid)
        return 0;

    auto gs = SGLD("valueAt", s->L);
    /*
     * So: make the stack my evaluation func then my table; then push my table
     * values; then call my function; then update my global
     */
    lua_getglobal(s->L, s->funcName);
    if (lua_isnil(s->L, -1))
    {
        s->isvalid = false;
        lua_pop(s->L, 1);
        return 0;
    }
    lua_getglobal(s->L, s->stateName);
    // Stack is now func > table  so we can update the table
    lua_pushstring(s->L, "intphase");
    lua_pushinteger(s->L, phaseIntPart);
    lua_settable(s->L, -3);

    auto addn = [s](const char *q, float f) {
        lua_pushstring(s->L, q);
        lua_pushnumber(s->L, f);
        lua_settable(s->L, -3);
    };

    auto addnil = [s](const char *q) {
        lua_pushstring(s->L, q);
        lua_pushnil(s->L);
        lua_settable(s->L, -3);
    };

    addn("phase", phaseFracPart);
    addn("delay", s->del);
    addn("attack", s->a);
    addn("hold", s->h);
    addn("sustain", s->s);
    addn("release", s->r);
    addn("rate", s->rate);
    addn("amplitude", s->amp);
    addn("startphase", s->phase);
    addn("deform", s->deform);
    addn("tempo", s->tempo);
    addn("songpos", s->songpos);

    addnil("retrigger_AEG");
    addnil("retrigger_FEG");

    auto lres = lua_pcall(s->L, 1, 1, 0);
    // stack is now just the result
    if (lres == LUA_OK)
    {
        if (lua_isnumber(s->L, -1))
        {
            // OK so you returned a value. Just use it
            auto r = lua_tonumber(s->L, -1);
            lua_pop(s->L, 1);
            return r;
        }
        if (!lua_istable(s->L, -1))
        {
            s->adderror(
                "The return of your LUA function must be a number or table. Just return input with "
                "output set.");
            s->isvalid = false;
            lua_pop(s->L, 1);
            return 0;
        }
        // Store the value and keep it on top of the stack
        lua_setglobal(s->L, s->stateName);
        lua_getglobal(s->L, s->stateName);

        lua_pushstring(s->L, "output");
        lua_gettable(s->L, -2);
        // top of stack is now the result
        float res = 0.0;
        if (lua_isnumber(s->L, -1))
        {
            res = lua_tonumber(s->L, -1);
        }
        else
        {
            s->adderror("You must define the  'output' field in the returned table as a number");
            s->isvalid = false;
        };
        // pop the result and the function
        lua_pop(s->L, 1);

        auto getBoolDefault = [s](const char *n, bool def) -> bool {
            auto res = def;
            lua_pushstring(s->L, n);
            lua_gettable(s->L, -2);
            if (lua_isboolean(s->L, -1))
            {
                res = lua_toboolean(s->L, -1);
            }
            lua_pop(s->L, 1);
            return res;
        };

        s->useEnvelope = getBoolDefault("use_envelope", true);
        s->retrigger_AEG = getBoolDefault("retrigger_AEG", false);
        s->retrigger_FEG = getBoolDefault("retrigger_FEG", false);

        // Finally pop the table result
        lua_pop(s->L, 1);

        return res;
    }
    else
    {
        s->isvalid = false;
        std::ostringstream oss;
        oss << "Failed to evaluate 'process' function." << lua_tostring(s->L, -1);
        s->adderror(oss.str());
        return 0;
    }
}

void createInitFormula(FormulaModulatorStorage *fs)
{
    fs->setFormula(R"FN(function process(modstate)
    -- this is a short lua script for a modulator. it must define
    -- a function called 'process'. input will contain keys 'phase' 'intphase',
    -- 'deform'. You must set the output value and return it. See the manual for more.

    -- simple saw
    modstate["output"] = modstate["phase"] * 2 - 1
    return modstate
end)FN");
    fs->interpreter = FormulaModulatorStorage::LUA;
}
} // namespace Formula
} // namespace Surge