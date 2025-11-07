// Universal Material Code (UMC) - Prototipo completo (C++17 single-file)
// NOTA: prototipo; extendable. No usa dependencias externas.

#include <bits/stdc++.h>
using namespace std;

/*
High-level design:

- Mini DSL "UCode" (very small): supports expressions, functions, mix, texture samples.
- AST nodes => Compiler => Bytecode instructions.
- VM for CPU-side preview: stack machine executing float vectors (vec3 + floats).
- Shader generator: linear translation of bytecode to GLSL/HLSL strings.
- Serializer: package bytecode + const data to binary buffer.
*/

// -----------------------------
// Math helpers (Vec3, etc.)
// -----------------------------
struct Vec3 {
    float x,y,z;
    Vec3(float a=0,float b=0,float c=0):x(a),y(b),z(c){}
};
inline Vec3 operator+(const Vec3&a,const Vec3&b){return Vec3(a.x+b.x,a.y+b.y,a.z+b.z);}
inline Vec3 operator-(const Vec3&a,const Vec3&b){return Vec3(a.x-b.x,a.y-b.y,a.z-b.z);}
inline Vec3 operator*(const Vec3&a,float s){return Vec3(a.x*s,a.y*s,a.z*s);}
inline Vec3 operator*(float s,const Vec3&a){return a*s;}
inline Vec3 operator*(const Vec3&a,const Vec3&b){return Vec3(a.x*b.x,a.y*b.y,a.z*b.z);}
inline Vec3 clamp01(const Vec3&v){return Vec3(min(max(v.x,0.0f),1.0f),min(max(v.y,0.0f),1.0f),min(max(v.z,0.0f),1.0f));}

// -----------------------------
// Bytecode definition
// -----------------------------
enum OpCode : uint8_t {
    OP_NOP = 0,
    OP_PUSH_CONST_VEC3,   // push vec3 constant (index)
    OP_PUSH_CONST_FLOAT,  // push float constant (index)
    OP_SAMPLE_TEX2D,      // sample texture (uv) -> vec3; takes textureID, uv from stack
    OP_ADD,               // vec3 + vec3 -> vec3
    OP_SUB,               // vec3 - vec3
    OP_MUL,               // vec3 * vec3  OR vec3 * float (type-flexible)
    OP_MULF,              // vec3 * float (explicit)
    OP_MIX,               // mix(a,b,t) -> vec3 (3 args)
    OP_DOT,               // dot(vec3,vec3) -> float
    OP_NORMALIZE,         // normalize(vec3)
    OP_PUSH_PARAM_FLOAT,  // push material parameter (float) by index
    OP_PUSH_PARAM_VEC3,   // push material parameter (vec3) by index
    OP_STORE_OUT_VEC3,    // store top vec3 into output slot (albedo, normal, mrao)
    OP_CALL_BUILTIN,      // call small builtin (id) pop args push result
    OP_END
};

struct Bytecode {
    vector<uint8_t> code;
    vector<Vec3> constVec3;
    vector<float> constFloat;
    vector<string> texNames; // texture identifiers
    vector<Vec3> paramsVec3; // material parameters (DNA)
    vector<float> paramsFloat;
};

// -----------------------------
// Simple DSL Parser (very small, expression style)
// Example usage string:
//
// baseColor = mix(vec3(0.8,0.1,0.1), texture("albedo", uv), 0.25);
// normal = normalize( texture("normal", uv) * vec3(2.0) - vec3(1.0) );
// roughness = 0.2 + noise(uv)*0.1;
//
// We'll implement a tiny parser supporting:
//
// identifiers, numbers, vec3(...), texture("name", uv), mix(a,b,t), normalize(v), noise(uv)
// assignments to "albedo","normal","mrao" outputs.
// -----------------------------

// Tokenizer:
enum TokenType {TK_EOF, TK_IDENT, TK_NUMBER, TK_COMMA, TK_LP, TK_RP, TK_SEMI, TK_EQ, TK_STR};
struct Token {
    TokenType type;
    string text;
};
struct Lexer {
    string s; size_t i=0; Token cur;
    Lexer(const string&src):s(src),i(0){ next(); }
    void skip_ws(){ while(i<s.size() && isspace((unsigned char)s[i])) ++i; }
    Token make(TokenType t, string txt=""){ Token tk={t,txt}; return tk; }
    void next(){
        skip_ws();
        if(i>=s.size()){ cur = make(TK_EOF); return; }
        char c = s[i];
        if(isalpha((unsigned char)c) || c=='_'){
            size_t j=i; while(j<s.size() && (isalnum((unsigned char)s[j])||s[j]=='_'||s[j]=='.')) ++j;
            cur = make(TK_IDENT, s.substr(i,j-i)); i=j; return;
        }
        if(isdigit((unsigned char)c) || c=='.'){
            size_t j=i; bool dot=false;
            while(j<s.size() && (isdigit((unsigned char)s[j])|| (!dot && s[j]=='.'))){
                if(s[j]=='.') dot=true;
                ++j;
            }
            cur = make(TK_NUMBER, s.substr(i,j-i)); i=j; return;
        }
        if(c=='"'){
            size_t j=i+1; while(j<s.size() && s[j]!='"') ++j;
            string inside = s.substr(i+1, j-(i+1));
            cur = make(TK_STR, inside); i = j+1; return;
        }
        i++;
        switch(c){
            case ',': cur=make(TK_COMMA,","); break;
            case '(': cur=make(TK_LP,"("); break;
            case ')': cur=make(TK_RP,")"); break;
            case ';': cur=make(TK_SEMI,";"); break;
            case '=': cur=make(TK_EQ,"="); break;
            default: cur=make(TK_IDENT,string(1,c)); break;
        }
    }
};

// AST minimal nodes
struct ASTNode {
    virtual ~ASTNode()=default;
};
using ASTPtr = unique_ptr<ASTNode>;
struct ExprNode : ASTNode {};
struct StmtNode : ASTNode {};

struct ExprConstVec3 : ExprNode { Vec3 v; ExprConstVec3(const Vec3&vv):v(vv){} };
struct ExprConstFloat : ExprNode { float v; ExprConstFloat(float vv):v(vv){} };
struct ExprVar : ExprNode { string name; ExprVar(string n):name(move(n)){} };
struct ExprCall : ExprNode { string fname; vector<ASTPtr> args; ExprCall(string f):fname(move(f)){} };
struct ExprTexture : ExprNode { string texname; ASTPtr uv; ExprTexture(string t, ASTPtr u):texname(move(t)),uv(move(u)){} };

struct StmtAssign : StmtNode { string target; ASTPtr expr; StmtAssign(string t, ASTPtr e):target(move(t)),expr(move(e)){} };

// Parser (very small)
struct Parser {
    Lexer lx;
    Parser(const string&s):lx(s){}
    Token cur(){ return lx.cur; }
    void next(){ lx.next(); }
    bool accept(TokenType t){ if(cur().type==t){ next(); return true;} return false;}
    bool expect(TokenType t){ if(cur().type==t){ next(); return true;} throw runtime_error("parse: unexpected token "+cur().text);}

    // parse expression (handles calls, vars, numbers, vec3(...))
    ASTPtr parseExpr(){
        Token tk = cur();
        if(tk.type==TK_NUMBER){
            float v = stof(tk.text); next();
            return make_unique<ExprConstFloat>(v);
        }
        if(tk.type==TK_IDENT){
            string id = tk.text; next();
            if(accept(TK_LP)){
                // function call
                // if id == vec3 then parse three numbers
                auto call = make_unique<ExprCall>(id);
                if(id=="vec3"){
                    // parse three args
                    for(int i=0;i<3;++i){
                        call->args.push_back(parseExpr());
                        if(i<2) expect(TK_COMMA);
                    }
                } else if(id=="texture"){
                    // special handling: texture("name", uv)
                    // next token could be string or ident
                    Token t2 = cur();
                    if(t2.type==TK_STR){
                        string tex = t2.text; next();
                        expect(TK_COMMA);
                        ASTPtr uv = parseExpr();
                        expect(TK_RP);
                        return make_unique<ExprTexture>(tex, move(uv));
                    } else throw runtime_error("texture expects string name");
                } else {
                    // generic call: parse comma separated args
                    if(cur().type!=TK_RP){
                        while(true){
                            call->args.push_back(parseExpr());
                            if(accept(TK_COMMA)) continue;
                            break;
                        }
                    }
                }
                expect(TK_RP);
                return call;
            } else {
                // variable
                return make_unique<ExprVar>(id);
            }
        }
        if(tk.type==TK_STR){
            // string literal (rare)
            string s = tk.text; next();
            // return as var?
            return make_unique<ExprVar>(s);
        }
        throw runtime_error("parseExpr: unexpected token "+tk.text);
    }

    // parse statement assignment: IDENT = expr;
    unique_ptr<StmtAssign> parseStmt(){
        Token t = cur(); if(t.type!=TK_IDENT) throw runtime_error("expected identifier at stmt start");
        string name = t.text; next();
        expect(TK_EQ);
        ASTPtr expr = parseExpr();
        expect(TK_SEMI);
        return make_unique<StmtAssign>(name, move(expr));
    }

    // parse program: multiple stmts
    vector<unique_ptr<StmtAssign>> parseProgram(){
        vector<unique_ptr<StmtAssign>> out;
        while(cur().type!=TK_EOF){
            out.push_back(parseStmt());
        }
        return out;
    }
};

// -----------------------------
// Compiler: AST -> Bytecode
// We'll support only producing outputs into three named outputs:
// "albedo", "normal", "mrao" (metallic, roughness, ao packed into vec3 or float channels).
// -----------------------------
struct Compiler {
    Bytecode bc;
    // helper maps for constants/params
    unordered_map<string,int> texNameToId;
    unordered_map<Vec3,int> constVec3Index; // not used as key easily; we will search
    vector<Vec3> tempVec3Consts;
    unordered_map<float,int> constFloatIndex;
    vector<float> tempFloatConsts;

    // params mapping
    unordered_map<string,int> paramVec3Index;
    unordered_map<string,int> paramFloatIndex;

    // program emission helpers
    void emitByte(uint8_t b){ bc.code.push_back(b); }
    void emitU32(uint32_t v){ // little endian
        for(int i=0;i<4;++i) bc.code.push_back((v>>(i*8))&0xFF);
    }
    int addConstVec3(const Vec3&v){
        for(size_t i=0;i<bc.constVec3.size();++i) if(bc.constVec3[i].x==v.x && bc.constVec3[i].y==v.y && bc.constVec3[i].z==v.z) return (int)i;
        bc.constVec3.push_back(v);
        return (int)bc.constVec3.size()-1;
    }
    int addConstFloat(float f){
        for(size_t i=0;i<bc.constFloat.size();++i) if(bc.constFloat[i]==f) return (int)i;
        bc.constFloat.push_back(f);
        return (int)bc.constFloat.size()-1;
    }
    int addTexName(const string& s){
        auto it=texNameToId.find(s);
        if(it!=texNameToId.end()) return it->second;
        bc.texNames.push_back(s);
        int id = (int)bc.texNames.size()-1;
        texNameToId[s]=id;
        return id;
    }
    int addParamVec3(const string& name, const Vec3&v){
        auto it=paramVec3Index.find(name);
        if(it!=paramVec3Index.end()) return it->second;
        bc.paramsVec3.push_back(v);
        int id=(int)bc.paramsVec3.size()-1;
        paramVec3Index[name]=id;
        return id;
    }
    int addParamFloat(const string& name, float f){
        auto it=paramFloatIndex.find(name);
        if(it!=paramFloatIndex.end()) return it->second;
        bc.paramsFloat.push_back(f);
        int id=(int)bc.paramsFloat.size()-1;
        paramFloatIndex[name]=id;
        return id;
    }

    // compile expression recursively -> emit bytecode that pushes result on stack
    void compileExpr(ExprNode* expr){
        if(auto p = dynamic_cast<ExprConstVec3*>(expr)){
            int idx = addConstVec3(p->v);
            emitByte(OP_PUSH_CONST_VEC3); emitU32((uint32_t)idx);
            return;
        }
        if(auto p = dynamic_cast<ExprConstFloat*>(expr)){
            int idx = addConstFloat(p->v);
            emitByte(OP_PUSH_CONST_FLOAT); emitU32((uint32_t)idx);
            return;
        }
        if(auto p = dynamic_cast<ExprVar*>(expr)){
            // variable could be uv, some param, or builtins
            if(p->name=="uv"){
                // uv is expected to be available at shader stage as builtin; we treat as paramVec3 "uv"
                int pid = addParamVec3("uv", Vec3(0,0,0));
                emitByte(OP_PUSH_PARAM_VEC3); emitU32((uint32_t)pid);
                return;
            }
            // Could be parameter
            // We'll allow params like material.baseColor.rgb or simply "tint"
            // For simplicity, treat any unknown ident as paramFloat or paramVec3 guess: try float param first
            // Here we assume user defined params preset in compiler step. For prototype, push 0.
            int pid = addParamFloat(p->name, 0.0f);
            emitByte(OP_PUSH_PARAM_FLOAT); emitU32((uint32_t)pid);
            return;
        }
        if(auto p = dynamic_cast<ExprTexture*>(expr)){
            // compile uv then issue sample
            compileExpr(p->uv.get()); // pushes uv vec3
            int tid = addTexName(p->texname);
            emitByte(OP_SAMPLE_TEX2D); emitU32((uint32_t)tid);
            return;
        }
        if(auto p = dynamic_cast<ExprCall*>(expr)){
            // common builtin handling: vec3, mix, normalize, dot, noise, normalize, etc.
            string f = p->fname;
            if(f=="vec3"){
                // args: x,y,z (numbers or floats)
                // compile 3 args -> push three floats and then build vec3 via push const vec3
                // But our VM simpler: evaluate by pushing const vec3 directly
                // we'll evaluate args if constants
                float x=0,y=0,z=0;
                // Attempt to evaluate args if constant floats
                bool allConst = true;
                for(int i=0;i<3;++i){
                    if(auto c = dynamic_cast<ExprConstFloat*>(p->args[i].get())) {
                        if(i==0) x=c->v; if(i==1) y=c->v; if(i==2) z=c->v;
                    } else allConst=false;
                }
                if(allConst){
                    int idx = addConstVec3(Vec3(x,y,z));
                    emitByte(OP_PUSH_CONST_VEC3); emitU32((uint32_t)idx);
                    return;
                } else {
                    // fallback: compile each arg, then call builtin to combine (we didn't implement)
                    throw runtime_error("vec3 with non-const args not supported in compiler prototype");
                }
            }
            if(f=="mix"){
                // mix(a,b,t) -> compile a b t then OP_MIX
                if(p->args.size()!=3) throw runtime_error("mix needs 3 args");
                compileExpr(p->args[0].get());
                compileExpr(p->args[1].get());
                compileExpr(p->args[2].get());
                emitByte(OP_MIX);
                return;
            }
            if(f=="normalize"){
                if(p->args.size()!=1) throw runtime_error("normalize needs 1 arg");
                compileExpr(p->args[0].get());
                emitByte(OP_NORMALIZE);
                return;
            }
            if(f=="dot"){
                if(p->args.size()!=2) throw runtime_error("dot needs 2 args");
                compileExpr(p->args[0].get());
                compileExpr(p->args[1].get());
                emitByte(OP_DOT);
                return;
            }
            if(f=="noise" || f=="perlin" || f=="fbm"){
                // For prototype: call builtin noise evaluator: OP_CALL_BUILTIN with id
                // compile uv arg
                if(p->args.size()!=1) throw runtime_error("noise needs 1 arg");
                compileExpr(p->args[0].get());
                emitByte(OP_CALL_BUILTIN); emitU32(1); // 1 = noise
                return;
            }
            // unknown call: try builtin id mapping
            if(f=="texture2D" || f=="texture"){ // handled above
                // unreachable here
                return;
            }
            // fallback: not supported
            throw runtime_error("unsupported function: "+f);
        }
        throw runtime_error("compileExpr: unknown expr type");
    }

    // compile statement assignment target="albedo"/"normal"/"mrao" etc.
    void compileStmt(StmtAssign* stmt){
        // compile RHS then emit store out
        compileExpr(stmt->expr.get());
        // store into output slot mapping
        if(stmt->target=="albedo"){
            emitByte(OP_STORE_OUT_VEC3); emitU32(0);
        } else if(stmt->target=="normal"){
            emitByte(OP_STORE_OUT_VEC3); emitU32(1);
        } else if(stmt->target=="mrao"){
            emitByte(OP_STORE_OUT_VEC3); emitU32(2);
        } else {
            // allow user params assignment? for now ignore
            // For generality, treat unknown as param set => not supported here
            throw runtime_error("unsupported assignment target: "+stmt->target);
        }
    }

    // compile full program (list of statements)
    Bytecode compileProgram(const vector<unique_ptr<StmtAssign>>& stmts){
        bc.code.clear(); bc.constVec3.clear(); bc.constFloat.clear(); bc.texNames.clear();
        for(auto &s : stmts){
            compileStmt(s.get());
        }
        emitByte(OP_END);
        return bc;
    }
};

// -----------------------------
// VM - Stack machine to execute bytecode for CPU preview.
// We'll keep a simple stack of variant types: either Vec3 or float.
// -----------------------------
struct VMValue {
    enum Type{VEC3, FLOAT} type;
    Vec3 v;
    float f;
    VMValue():type(FLOAT),v(),f(0.0f){}
    static VMValue fromVec(const Vec3&vv){ VMValue x; x.type=VEC3; x.v=vv; return x;}
    static VMValue fromFloat(float ff){ VMValue x; x.type=FLOAT; x.f=ff; return x;}
};
struct VM {
    Bytecode *bc;
    // External callbacks: texture sampler and builtins
    function<Vec3(const string&, const Vec3&uv)> texSampler;
    function<float(const Vec3&uv)> noiseFn;
    VM(Bytecode* b=nullptr):bc(b){
        // default stubs
        texSampler = [](const string&name,const Vec3&uv)->Vec3{
            // default: color by name hash for debug
            size_t h=0; for(char c:name) h = h*131 + (unsigned char)c;
            float t = (h%255)/255.0f;
            return Vec3(t,t,t);
        };
        noiseFn = [](const Vec3&uv)->float{ return 0.0f; };
    }

    Vec3 executeSample(){
        if(!bc) throw runtime_error("VM: no bytecode");
        vector<VMValue> stack;
        Vec3 outAlbedo(0,0,0), outNormal(0,0,0), outMRAO(0,0,0);
        size_t ip=0;
        auto readU32 = [&](uint32_t &out){
            if(ip+4>bc->code.size()) throw runtime_error("bytecode truncated");
            uint32_t v=0;
            for(int k=0;k<4;++k) v |= ((uint32_t)bc->code[ip++]) << (k*8);
            out=v;
        };
        while(ip<bc->code.size()){
            OpCode op = (OpCode)bc->code[ip++];
            switch(op){
                case OP_NOP: break;
                case OP_PUSH_CONST_VEC3:{
                    uint32_t idx; readU32(idx);
                    if(idx>=bc->constVec3.size()) throw runtime_error("const vec idx");
                    stack.push_back(VMValue::fromVec(bc->constVec3[idx]));
                } break;
                case OP_PUSH_CONST_FLOAT:{
                    uint32_t idx; readU32(idx);
                    if(idx>=bc->constFloat.size()) throw runtime_error("const float idx");
                    stack.push_back(VMValue::fromFloat(bc->constFloat[idx]));
                } break;
                case OP_PUSH_PARAM_FLOAT:{
                    uint32_t idx; readU32(idx);
                    float val = (idx<bc->paramsFloat.size())?bc->paramsFloat[idx]:0.0f;
                    stack.push_back(VMValue::fromFloat(val));
                } break;
                case OP_PUSH_PARAM_VEC3:{
                    uint32_t idx; readU32(idx);
                    Vec3 val = (idx<bc->paramsVec3.size())?bc->paramsVec3[idx]:Vec3(0,0,0);
                    stack.push_back(VMValue::fromVec(val));
                } break;
                case OP_SAMPLE_TEX2D:{
                    uint32_t tid; readU32(tid);
                    if(tid>=bc->texNames.size()) throw runtime_error("tex id oob");
                    // uv from stack
                    if(stack.empty()) throw runtime_error("stack underflow samp");
                    VMValue uvv = stack.back(); stack.pop_back();
                    Vec3 uv = (uvv.type==VMValue::VEC3)?uvv.v:Vec3(uvv.f,0,0);
                    Vec3 col = texSampler(bc->texNames[tid], uv);
                    stack.push_back(VMValue::fromVec(col));
                } break;
                case OP_ADD:{
                    auto b = stack.back(); stack.pop_back();
                    auto a = stack.back(); stack.pop_back();
                    if(a.type==VMValue::VEC3 && b.type==VMValue::VEC3) stack.push_back(VMValue::fromVec(a.v + b.v));
                    else throw runtime_error("ADD type mismatch");
                } break;
                case OP_SUB:{
                    auto b = stack.back(); stack.pop_back();
                    auto a = stack.back(); stack.pop_back();
                    if(a.type==VMValue::VEC3 && b.type==VMValue::VEC3) stack.push_back(VMValue::fromVec(a.v - b.v));
                    else throw runtime_error("SUB type mismatch");
                } break;
                case OP_MUL:{
                    auto b = stack.back(); stack.pop_back();
                    auto a = stack.back(); stack.pop_back();
                    if(a.type==VMValue::VEC3 && b.type==VMValue::VEC3) stack.push_back(VMValue::fromVec(a.v * b.v));
                    else if(a.type==VMValue::VEC3 && b.type==VMValue::FLOAT) stack.push_back(VMValue::fromVec(a.v * b.f));
                    else if(a.type==VMValue::FLOAT && b.type==VMValue::VEC3) stack.push_back(VMValue::fromVec(b.v * a.f));
                    else if(a.type==VMValue::FLOAT && b.type==VMValue::FLOAT) stack.push_back(VMValue::fromFloat(a.f * b.f));
                    else throw runtime_error("MUL unsupported types");
                } break;
                case OP_MULF:{
                    auto b = stack.back(); stack.pop_back();
                    auto a = stack.back(); stack.pop_back();
                    float f = (b.type==VMValue::FLOAT)?b.f: (b.type==VMValue::VEC3?b.v.x:0.0f);
                    if(a.type==VMValue::VEC3) stack.push_back(VMValue::fromVec(a.v * f));
                    else throw runtime_error("MULF unsupported");
                } break;
                case OP_MIX:{
                    // a b t -> mix(a,b,t)
                    auto t = stack.back(); stack.pop_back();
                    auto bb = stack.back(); stack.pop_back();
                    auto aa = stack.back(); stack.pop_back();
                    if(aa.type==VMValue::VEC3 && bb.type==VMValue::VEC3 && t.type==VMValue::FLOAT){
                        Vec3 r = aa.v*(1.0f - t.f) + bb.v*(t.f);
                        stack.push_back(VMValue::fromVec(r));
                    } else throw runtime_error("MIX types");
                } break;
                case OP_DOT:{
                    auto b = stack.back(); stack.pop_back();
                    auto a = stack.back(); stack.pop_back();
                    if(a.type==VMValue::VEC3 && b.type==VMValue::VEC3){
                        float d = a.v.x*b.v.x + a.v.y*b.v.y + a.v.z*b.v.z;
                        stack.push_back(VMValue::fromFloat(d));
                    } else throw runtime_error("DOT types");
                } break;
                case OP_NORMALIZE:{
                    auto a = stack.back(); stack.pop_back();
                    if(a.type==VMValue::VEC3){
                        Vec3 v=a.v;
                        float len = sqrtf(max(1e-12f, v.x*v.x+v.y*v.y+v.z*v.z));
                        stack.push_back(VMValue::fromVec(Vec3(v.x/len, v.y/len, v.z/len)));
                    } else throw runtime_error("NORMALIZE expects vec3");
                } break;
                case OP_CALL_BUILTIN:{
                    uint32_t id; readU32(id);
                    if(id==1){
                        // noise: pop uv vec3 push float
                        auto uvv = stack.back(); stack.pop_back();
                        Vec3 uv = (uvv.type==VMValue::VEC3)?uvv.v:Vec3(uvv.f,0,0);
                        float n = noiseFn(uv);
                        stack.push_back(VMValue::fromFloat(n));
                    } else throw runtime_error("unknown builtin id");
                } break;
                case OP_STORE_OUT_VEC3:{
                    uint32_t slot; readU32(slot);
                    auto v = stack.back(); stack.pop_back();
                    if(v.type!=VMValue::VEC3) throw runtime_error("store_out expects vec3");
                    if(slot==0) outAlbedo=v.v;
                    else if(slot==1) outNormal=v.v;
                    else if(slot==2) outMRAO=v.v;
                } break;
                case OP_END: {
                    // end execution
                    ip = bc->code.size(); continue;
                } break;
                default:
                    throw runtime_error("VM: unknown opcode "+to_string((int)op));
            }
        }
        // return albedo as sample result for demo
        return outAlbedo;
    }
};

// -----------------------------
// Shader generator: translate bytecode into GLSL or HLSL code string.
// We'll generate a fragment function that produces albedo/normal/mrao outputs.
// -----------------------------
struct ShaderGen {
    Bytecode *bc;
    ShaderGen(Bytecode*b=nullptr):bc(b){}
    string genGLSL(){
        // header and uniforms
        string s;
        s += "#version 330 core\n";
        s += "in vec2 v_uv;\n";
        s += "out vec4 o_albedo;\n";
        s += "out vec3 o_normal;\n";
        s += "out vec3 o_mrao;\n";
        // declare textures
        for(size_t i=0;i<bc->texNames.size();++i){
            s += "uniform sampler2D u_tex_" + to_string(i) + "; // " + bc->texNames[i] + "\n";
        }
        s += "uniform vec3 u_paramVec3[" + to_string(bc->paramsVec3.size()) + "];\n";
        s += "uniform float u_paramFloat[" + to_string(bc->paramsFloat.size()) + "];\n";
        s += "\nvec3 sampleTex(int texid, vec2 uv){\n";
        s += "  if(texid==0) return texture(u_tex_0, uv).rgb;\n";
        // generate cascade for other textures
        for(size_t i=1;i<bc->texNames.size();++i){
            s += "  if(texid=="+to_string(i)+") return texture(u_tex_"+to_string(i)+", uv).rgb;\n";
        }
        s += "  return vec3(0.0);\n}\n\n";

        s += "void main(){\n";
        s += " vec3 _stackVec[64]; int _sp=0;\n";
        s += " float _stackF[64]; int _fsp=0;\n";

        // We'll iterate bytecode and generate code that simulates stack operations
        size_t ip=0;
        auto readU32 = [&](uint32_t &out)->bool{
            if(ip+4>bc->code.size()) return false;
            uint32_t v=0;
            for(int k=0;k<4;++k) v |= ((uint32_t)bc->code[ip++]) << (k*8);
            out=v; return true;
        };
        while(ip<bc->code.size()){
            OpCode op = (OpCode)bc->code[ip++];
            switch(op){
                case OP_PUSH_CONST_VEC3:{
                    uint32_t idx; readU32(idx);
                    Vec3 v = bc->constVec3[idx];
                    s += " _stackVec[_sp++] = vec3(" + to_string(v.x) + "f," + to_string(v.y) + "f," + to_string(v.z) + "f);\n";
                } break;
                case OP_PUSH_CONST_FLOAT:{
                    uint32_t idx; readU32(idx);
                    float f = bc->constFloat[idx];
                    s += " _stackF[_fsp++] = " + to_string(f) + "f;\n";
                } break;
                case OP_PUSH_PARAM_VEC3:{
                    uint32_t idx; readU32(idx);
                    s += " _stackVec[_sp++] = u_paramVec3[" + to_string(idx) + "];\n";
                } break;
                case OP_PUSH_PARAM_FLOAT:{
                    uint32_t idx; readU32(idx);
                    s += " _stackF[_fsp++] = u_paramFloat[" + to_string(idx) + "];\n";
                } break;
                case OP_SAMPLE_TEX2D:{
                    uint32_t tid; readU32(tid);
                    // pop uv from vec stack
                    s += " {\n vec2 _uv = vec2(_stackVec[--_sp].x, _stackVec[_sp].y);\n _stackVec[_sp] = sampleTex(" + to_string(tid) + ", _uv); _sp++; }\n";
                } break;
                case OP_ADD:{
                    s += " { vec3 b=_stackVec[--_sp]; vec3 a=_stackVec[--_sp]; _stackVec[_sp++] = a+b; }\n";
                } break;
                case OP_SUB:{
                    s += " { vec3 b=_stackVec[--_sp]; vec3 a=_stackVec[--_sp]; _stackVec[_sp++] = a-b; }\n";
                } break;
                case OP_MUL:{
                    // generic: if fstack top then vec*float else vec*vec
                    s += " { // MUL generic: assume vec*vec or vec*float\n";
                    s += "  // pick by SP/FSP counts (simple heuristic)\n";
                    s += "  if(_fsp>0){ float fb=_stackF[--_fsp]; vec3 a=_stackVec[--_sp]; _stackVec[_sp++] = a*fb; }\n";
                    s += "  else { vec3 b=_stackVec[--_sp]; vec3 a=_stackVec[--_sp]; _stackVec[_sp++] = a*b; }\n";
                    s += " }\n";
                } break;
                case OP_MIX:{
                    s += " { float t=_stackF[--_fsp]; vec3 bb=_stackVec[--_sp]; vec3 aa=_stackVec[--_sp]; _stackVec[_sp++] = mix(aa,bb,t); }\n";
                } break;
                case OP_DOT:{
                    s += " { vec3 b=_stackVec[--_sp]; vec3 a=_stackVec[--_sp]; _stackF[_fsp++] = dot(a,b); }\n";
                } break;
                case OP_NORMALIZE:{
                    s += " { vec3 a=_stackVec[--_sp]; _stackVec[_sp++] = normalize(a); }\n";
                } break;
                case OP_CALL_BUILTIN:{
                    uint32_t id; readU32(id);
                    if(id==1){
                        // noise: pop uv vec3 -> push float
                        s += " { vec3 uv = _stackVec[--_sp]; _stackF[_fsp++] = fract(sin(dot(uv.xy ,vec2(12.9898,78.233))) * 43758.5453); }\n";
                    }
                } break;
                case OP_STORE_OUT_VEC3:{
                    uint32_t slot; readU32(slot);
                    if(slot==0) s += " o_albedo = vec4(_stackVec[--_sp],1.0);\n";
                    else if(slot==1) s += " o_normal = _stackVec[--_sp];\n";
                    else if(slot==2) s += " o_mrao = _stackVec[--_sp];\n";
                } break;
                case OP_END:{
                    ip = bc->code.size();
                } break;
                default:
                    // ignore other ops for now
                    break;
            }
        }
        s += "}\n";
        return s;
    }
};

// -----------------------------
// Serialization of Bytecode package (very simple)
// Format:
// [magic 4 bytes] [version u32] [sizes...] then sections: constVec3, constFloat, texNames, paramsVec3, paramsFloat, code
// -----------------------------
vector<uint8_t> serializeBytecode(const Bytecode& bc){
    vector<uint8_t> out;
    auto pushU32 = [&](uint32_t v){ for(int k=0;k<4;++k) out.push_back((v>>(k*8))&0xFF); };
    auto pushFloat = [&](float f){
        uint32_t v; memcpy(&v,&f,sizeof(float));
        pushU32(v);
    };
    // header
    out.insert(out.end(), {'U','M','C','1'});
    pushU32(1); // version
    // constVec3
    pushU32((uint32_t)bc.constVec3.size());
    for(auto &v:bc.constVec3){ pushFloat(v.x); pushFloat(v.y); pushFloat(v.z); }
    // constFloat
    pushU32((uint32_t)bc.constFloat.size());
    for(auto f:bc.constFloat) pushFloat(f);
    // texNames
    pushU32((uint32_t)bc.texNames.size());
    for(auto &t:bc.texNames){
        pushU32((uint32_t)t.size());
        out.insert(out.end(), t.begin(), t.end());
    }
    // paramsVec3
    pushU32((uint32_t)bc.paramsVec3.size());
    for(auto &v:bc.paramsVec3){ pushFloat(v.x); pushFloat(v.y); pushFloat(v.z); }
    // paramsFloat
    pushU32((uint32_t)bc.paramsFloat.size());
    for(auto f:bc.paramsFloat) pushFloat(f);
    // code size
    pushU32((uint32_t)bc.code.size());
    out.insert(out.end(), bc.code.begin(), bc.code.end());
    return out;
}
Bytecode deserializeBytecode(const vector<uint8_t>& in){
    size_t ip=0; auto readU32=[&]()->uint32_t{ uint32_t v=0; for(int k=0;k<4;++k) v |= ((uint32_t)in[ip++]) << (k*8); return v; };
    auto readFloat=[&]()->float{ uint32_t v=readU32(); float f; memcpy(&f,&v,sizeof(float)); return f; };
    if(in.size()<4) throw runtime_error("bad package");
    if(!(in[0]=='U' && in[1]=='M' && in[2]=='C' && in[3]=='1')) throw runtime_error("not umc");
    ip=4; uint32_t ver = readU32();
    Bytecode bc;
    uint32_t nVec = readU32();
    for(uint32_t i=0;i<nVec;++i){ float x=readFloat(), y=readFloat(), z=readFloat(); bc.constVec3.emplace_back(x,y,z); }
    uint32_t nF = readU32();
    for(uint32_t i=0;i<nF;++i) bc.constFloat.push_back(readFloat());
    uint32_t nT = readU32();
    for(uint32_t i=0;i<nT;++i){ uint32_t l = readU32(); string t; t.resize(l); for(uint32_t j=0;j<l;++j) t[j]=in[ip++]; bc.texNames.push_back(t); }
    uint32_t nPV = readU32();
    for(uint32_t i=0;i<nPV;++i){ float x=readFloat(), y=readFloat(), z=readFloat(); bc.paramsVec3.emplace_back(x,y,z); }
    uint32_t nPF = readU32();
    for(uint32_t i=0;i<nPF;++i) bc.paramsFloat.push_back(readFloat());
    uint32_t codeSz = readU32();
    for(uint32_t i=0;i<codeSz;++i) bc.code.push_back(in[ip++]);
    return bc;
}

// -----------------------------
// Example usage / demonstration
// -----------------------------
int main(){
    try {
        // Example material in mini UCode DSL
        string sampleProg = R"(
            albedo = mix(vec3(0.8,0.1,0.1), texture("albedo_diff", uv), 0.25);
            normal = normalize( texture("normal_map", uv) * vec3(2.0,2.0,1.0) - vec3(1.0,1.0,0.0) );
            mrao = vec3(0.1, 0.4, 1.0);
        )";

        cout << "Parsing program...\n";
        Parser p(sampleProg);
        auto stmts = p.parseProgram();
        cout << "Statements parsed: " << stmts.size() << "\n";

        cout << "Compiling...\n";
        Compiler comp;
        // add some material params as an example
        comp.addParamVec3("tint", Vec3(1,1,1));
        Bytecode bc = comp.compileProgram(stmts);

        cout << "Bytecode size: " << bc.code.size() << " bytes\n";
        cout << "ConstVec3 count: " << bc.constVec3.size() << "\n";
        cout << "ConstFloat count: " << bc.constFloat.size() << "\n";
        cout << "Texture names: \n";
        for(size_t i=0;i<bc.texNames.size();++i) cout << "  ["<<i<<"] "<<bc.texNames[i]<<"\n";

        // Serialize -> deserialize roundtrip
        auto blob = serializeBytecode(bc);
        cout << "Serialized package size: " << blob.size() << " bytes\n";
        Bytecode bc2 = deserializeBytecode(blob);
        cout << "Deserialized. Code size: " << bc2.code.size() << "\n";

        // VM preview
        VM vm(&bc2);
        // set a simple tex sampler
        vm.texSampler = [&](const string& name, const Vec3& uv)->Vec3{
            if(name=="albedo_diff") return Vec3(0.2f+uv.x*0.3f,0.2f+uv.y*0.1f,0.1f);
            if(name=="normal_map") return Vec3(0.5f+uv.x*0.1f,0.5f+uv.y*0.03f,1.0f);
            return Vec3(0.5f,0.5f,0.5f);
        };
        vm.noiseFn = [](const Vec3&uv){ return 0.0f; };
        // set UV param in paramsVec3 slot (we created one param "uv" with index 0)
        if(bc2.paramsVec3.size()==0) bc2.paramsVec3.push_back(Vec3(0.1f,0.2f,0.0f));
        vm.bc = &bc2;
        Vec3 sample = vm.executeSample();
        cout << "VM sample albedo: ("<<sample.x<<","<<sample.y<<","<<sample.z<<")\n";

        // Shader generation
        ShaderGen gen(&bc2);
        string glsl = gen.genGLSL();
        cout << "Generated GLSL:\n";
        cout << "---------------------------\n";
        cout << glsl.substr(0, min<size_t>(glsl.size(), 1200)) << "\n";
        cout << "---------------------------\n";
        cout << "GLSL length: " << glsl.size() << " chars\n";

        cout << "UMC prototype completed.\n";
    } catch(const exception &e){
        cerr << "Error: " << e.what() << "\n";
    }
    return 0;
}