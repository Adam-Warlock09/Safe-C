// Calls the bump_X functions of struct_by_value.safec and checks every field.
// Returns the number of the first shape that fails, 0 if all pass.
#include <stdbool.h>

struct OneInt { int a; };
struct OneChar { char a; };
struct OneBool { bool a; };
struct ThreeChars { char a; char b; char c; };
struct IntCharBool { int x; char c; bool b; };
struct CharInt { char a; int b; };
struct ThreeInts { int a; int b; int c; };
struct FourInts { int a; int b; int c; int d; };
struct OneFloat { float a; };
struct TwoFloats { float a; float b; };
struct ThreeFloats { float a; float b; float c; };
struct FloatInt { float a; int b; };
struct IntTwoFloats { int a; float b; float c; };
struct BoolFloat { bool a; float f; };
struct PtrInt { int* p; int a; };
struct PtrFloat { int* p; float f; };
struct TwoPtrs { int* p; int* q; };
struct FloatArray { float v[3]; };
struct CharArray5 { char s[5]; };
struct CharArray9 { char s[9]; };
struct IntArray3 { int v[3]; };
struct Nested { int a; struct TwoFloats f; };
struct Big { int a; int b; int c; int d; int e; };
struct PtrsAndInt { int* p; int* q; int a; };

struct OneInt bump_OneInt(struct OneInt x);
struct OneChar bump_OneChar(struct OneChar x);
struct OneBool bump_OneBool(struct OneBool x);
struct ThreeChars bump_ThreeChars(struct ThreeChars x);
struct IntCharBool bump_IntCharBool(struct IntCharBool x);
struct CharInt bump_CharInt(struct CharInt x);
struct ThreeInts bump_ThreeInts(struct ThreeInts x);
struct FourInts bump_FourInts(struct FourInts x);
struct OneFloat bump_OneFloat(struct OneFloat x);
struct TwoFloats bump_TwoFloats(struct TwoFloats x);
struct ThreeFloats bump_ThreeFloats(struct ThreeFloats x);
struct FloatInt bump_FloatInt(struct FloatInt x);
struct IntTwoFloats bump_IntTwoFloats(struct IntTwoFloats x);
struct BoolFloat bump_BoolFloat(struct BoolFloat x);
struct PtrInt bump_PtrInt(struct PtrInt x);
struct PtrFloat bump_PtrFloat(struct PtrFloat x);
struct TwoPtrs bump_TwoPtrs(struct TwoPtrs x);
struct FloatArray bump_FloatArray(struct FloatArray x);
struct CharArray5 bump_CharArray5(struct CharArray5 x);
struct CharArray9 bump_CharArray9(struct CharArray9 x);
struct IntArray3 bump_IntArray3(struct IntArray3 x);
struct Nested bump_Nested(struct Nested x);
struct Big bump_Big(struct Big x);
struct PtrsAndInt bump_PtrsAndInt(struct PtrsAndInt x);

static int target = 7, other = 9;

static int check_OneInt(void) {
    struct OneInt in = { 11000 };
    struct OneInt out = bump_OneInt(in);
    return out.a == in.a + 1;
}
static int check_OneChar(void) {
    struct OneChar in = { 68 };
    struct OneChar out = bump_OneChar(in);
    return out.a == (char)(in.a + 1);
}
static int check_OneBool(void) {
    struct OneBool in = { true };
    struct OneBool out = bump_OneBool(in);
    return out.a == !in.a;
}
static int check_ThreeChars(void) {
    struct ThreeChars in = { 66, 65, 64 };
    struct ThreeChars out = bump_ThreeChars(in);
    return out.a == (char)(in.a + 1)
        && out.b == (char)(in.b + 1)
        && out.c == (char)(in.c + 1);
}
static int check_IntCharBool(void) {
    struct IntCharBool in = { 17000, 62, true };
    struct IntCharBool out = bump_IntCharBool(in);
    return out.x == in.x + 1
        && out.c == (char)(in.c + 1)
        && out.b == !in.b;
}
static int check_CharInt(void) {
    struct CharInt in = { 60, 21000 };
    struct CharInt out = bump_CharInt(in);
    return out.a == (char)(in.a + 1)
        && out.b == in.b + 1;
}
static int check_ThreeInts(void) {
    struct ThreeInts in = { 22000, 23000, 24000 };
    struct ThreeInts out = bump_ThreeInts(in);
    return out.a == in.a + 1
        && out.b == in.b + 1
        && out.c == in.c + 1;
}
static int check_FourInts(void) {
    struct FourInts in = { 25000, 26000, 27000, 28000 };
    struct FourInts out = bump_FourInts(in);
    return out.a == in.a + 1
        && out.b == in.b + 1
        && out.c == in.c + 1
        && out.d == in.d + 1;
}
static int check_OneFloat(void) {
    struct OneFloat in = { 29.5f };
    struct OneFloat out = bump_OneFloat(in);
    return out.a == in.a + 1.0f;
}
static int check_TwoFloats(void) {
    struct TwoFloats in = { 30.5f, 31.5f };
    struct TwoFloats out = bump_TwoFloats(in);
    return out.a == in.a + 1.0f
        && out.b == in.b + 1.0f;
}
static int check_ThreeFloats(void) {
    struct ThreeFloats in = { 32.5f, 33.5f, 34.5f };
    struct ThreeFloats out = bump_ThreeFloats(in);
    return out.a == in.a + 1.0f
        && out.b == in.b + 1.0f
        && out.c == in.c + 1.0f;
}
static int check_FloatInt(void) {
    struct FloatInt in = { 35.5f, 36000 };
    struct FloatInt out = bump_FloatInt(in);
    return out.a == in.a + 1.0f
        && out.b == in.b + 1;
}
static int check_IntTwoFloats(void) {
    struct IntTwoFloats in = { 37000, 38.5f, 39.5f };
    struct IntTwoFloats out = bump_IntTwoFloats(in);
    return out.a == in.a + 1
        && out.b == in.b + 1.0f
        && out.c == in.c + 1.0f;
}
static int check_BoolFloat(void) {
    struct BoolFloat in = { false, 41.5f };
    struct BoolFloat out = bump_BoolFloat(in);
    return out.a == !in.a
        && out.f == in.f + 1.0f;
}
static int check_PtrInt(void) {
    struct PtrInt in = { &other, 43000 };
    struct PtrInt out = bump_PtrInt(in);
    return out.p == in.p
        && out.a == in.a + 1;
}
static int check_PtrFloat(void) {
    struct PtrFloat in = { &other, 45.5f };
    struct PtrFloat out = bump_PtrFloat(in);
    return out.p == in.p
        && out.f == in.f + 1.0f;
}
static int check_TwoPtrs(void) {
    struct TwoPtrs in = { &other, &target };
    struct TwoPtrs out = bump_TwoPtrs(in);
    return out.p == in.p
        && out.q == in.q;
}
static int check_FloatArray(void) {
    struct FloatArray in = { { 48.5f, 49.5f, 50.5f } };
    struct FloatArray out = bump_FloatArray(in);
    return out.v[0] == in.v[0] + 1.0f
        && out.v[1] == in.v[1] + 1.0f
        && out.v[2] == in.v[2] + 1.0f;
}
static int check_CharArray5(void) {
    struct CharArray5 in = { { 29, 28, 27, 26, 25 } };
    struct CharArray5 out = bump_CharArray5(in);
    return out.s[0] == (char)(in.s[0] + 1)
        && out.s[1] == (char)(in.s[1] + 1)
        && out.s[2] == (char)(in.s[2] + 1)
        && out.s[3] == (char)(in.s[3] + 1)
        && out.s[4] == (char)(in.s[4] + 1);
}
static int check_CharArray9(void) {
    struct CharArray9 in = { { 24, 23, 22, 21, 20, 19, 18, 17, 16 } };
    struct CharArray9 out = bump_CharArray9(in);
    return out.s[0] == (char)(in.s[0] + 1)
        && out.s[1] == (char)(in.s[1] + 1)
        && out.s[2] == (char)(in.s[2] + 1)
        && out.s[3] == (char)(in.s[3] + 1)
        && out.s[4] == (char)(in.s[4] + 1)
        && out.s[5] == (char)(in.s[5] + 1)
        && out.s[6] == (char)(in.s[6] + 1)
        && out.s[7] == (char)(in.s[7] + 1)
        && out.s[8] == (char)(in.s[8] + 1);
}
static int check_IntArray3(void) {
    struct IntArray3 in = { { 65000, 66000, 67000 } };
    struct IntArray3 out = bump_IntArray3(in);
    return out.v[0] == in.v[0] + 1
        && out.v[1] == in.v[1] + 1
        && out.v[2] == in.v[2] + 1;
}
static int check_Nested(void) {
    struct Nested in = { 68000, { 69.25f, -69.75f } };
    struct Nested out = bump_Nested(in);
    return out.a == in.a + 1
        && out.f.a == in.f.a + 1.0f
        && out.f.b == in.f.b + 1.0f;
}
static int check_Big(void) {
    struct Big in = { 70000, 71000, 72000, 73000, 74000 };
    struct Big out = bump_Big(in);
    return out.a == in.a + 1
        && out.b == in.b + 1
        && out.c == in.c + 1
        && out.d == in.d + 1
        && out.e == in.e + 1;
}
static int check_PtrsAndInt(void) {
    struct PtrsAndInt in = { &target, &other, 77000 };
    struct PtrsAndInt out = bump_PtrsAndInt(in);
    return out.p == in.p
        && out.q == in.q
        && out.a == in.a + 1;
}

int main(void) {
    if (!check_OneInt()) return 1;
    if (!check_OneChar()) return 2;
    if (!check_OneBool()) return 3;
    if (!check_ThreeChars()) return 4;
    if (!check_IntCharBool()) return 5;
    if (!check_CharInt()) return 6;
    if (!check_ThreeInts()) return 7;
    if (!check_FourInts()) return 8;
    if (!check_OneFloat()) return 9;
    if (!check_TwoFloats()) return 10;
    if (!check_ThreeFloats()) return 11;
    if (!check_FloatInt()) return 12;
    if (!check_IntTwoFloats()) return 13;
    if (!check_BoolFloat()) return 14;
    if (!check_PtrInt()) return 15;
    if (!check_PtrFloat()) return 16;
    if (!check_TwoPtrs()) return 17;
    if (!check_FloatArray()) return 18;
    if (!check_CharArray5()) return 19;
    if (!check_CharArray9()) return 20;
    if (!check_IntArray3()) return 21;
    if (!check_Nested()) return 22;
    if (!check_Big()) return 23;
    if (!check_PtrsAndInt()) return 24;
    return 0;
}
