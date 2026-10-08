// Checks the emulator's sce::Json implementation by calling it the way a title does: through the
// exported member functions, with objects that are nothing but storage of the right size.
//
//   clang-cl /std:c++latest /EHsc -fuse-ld=lld /I tools/tests/stubs /I shadps4-arm64-main/src
//       tools/tests/json_test.cpp shadps4-arm64-main/src/core/libraries/json/json.cpp
#include <cstdio>
#include <cstring>
#include <string>

#include "core/libraries/json/json.h"

using namespace Libraries::Json;

static int failures = 0;

static void Check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    failures += ok ? 0 : 1;
}

static std::string Text(const Value& value) {
    String out;
    StringConstruct(&out);
    ValueToString(&value, &out);
    std::string text = StringCStr(&out);
    StringDestruct(&out);
    return text;
}

static std::string Serialized(Value& value) {
    String out;
    StringConstruct(&out);
    ValueSerialize(&value, &out);
    std::string text = StringCStr(&out);
    StringDestruct(&out);
    return text;
}

int main() {
    // What the game's animation code parses.
    {
        const char* text = "[{\"delay\": 0.0, \"duration\": 0.5, \"anim\": \"idle_loop\", "
                           "\"link\": \"\"}, {\"delay\": 2, \"duration\": -3, \"anim\": "
                           "\"celebrate01\", \"link\": \"a\\tb\\u00e9\"}]";
        Value root;
        ValueConstruct(&root);
        Check(ParserParse(&root, text, std::strlen(text)) == 0, "array of objects parses");
        Check(ValueGetType(&root) == 6 && ValueCount(&root) == 2, "it is an array of two");
        const Value* first = ValueIndex(&root, 0);
        Check(ValueGetType(first) == 7, "its elements are objects");
        Check(ValueGetType(ValueMember(first, "delay")) == 4 &&
                  *ValueGetReal(ValueMember(first, "delay")) == 0.0,
              "0.0 is a real");
        Check(*ValueGetReal(ValueMember(first, "duration")) == 0.5, "0.5 reads back");
        Check(Text(*ValueMember(first, "anim")) == "idle_loop", "strings come out as they are");
        const Value* second = ValueIndex(&root, 1);
        Check(ValueGetType(ValueMember(second, "delay")) == 3 &&
                  *ValueGetInteger(ValueMember(second, "delay")) == 2 &&
                  *ValueGetReal(ValueMember(second, "delay")) == 2.0,
              "2 is an unsigned integer, readable as a real too");
        Check(ValueGetType(ValueMember(second, "duration")) == 2 &&
                  *ValueGetInteger(ValueMember(second, "duration")) == -3,
              "-3 is an integer");
        Check(Text(*ValueMember(second, "link")) == "a\tb\xc3\xa9", "escapes are resolved");
        Check(ValueGetType(ValueMember(second, "missing")) == 0, "a missing member is null");
        Check(ValueGetType(ValueIndex(&root, 9)) == 0, "an index past the end is null");

        // Copies are independent of what they were copied from.
        Value copy;
        ValueConstructCopy(&copy, &root);
        ValueDestruct(&root);
        Check(ValueCount(&copy) == 2 &&
                  Text(*ValueMember(ValueIndex(&copy, 1), "anim")) == "celebrate01",
              "a copy outlives its source");
        Value assigned;
        ValueConstruct(&assigned);
        ValueAssign(&assigned, ValueIndex(&copy, 0));
        ValueDestruct(&copy);
        Check(Text(*ValueMember(&assigned, "anim")) == "idle_loop", "so does an assigned element");
        ValueDestruct(&assigned);
    }

    // What the game's option code does: walk an object of flags.
    {
        const char* text = "{\"zeta\": true, \"alpha\": false, \"mid\": true}";
        Value root;
        ValueConstruct(&root);
        ParserParse(&root, text, std::strlen(text));
        Check(ValueToBool(&root), "an object converts to true");
        const Object* object = ValueGetObject(&root);
        Iterator at;
        Iterator end;
        ObjectBegin(&at, object);
        ObjectEnd(&end, object);
        std::string seen;
        while (IteratorNotEqual(&at, &end)) {
            Pair* pair = IteratorDereference(&at);
            seen += StringCStr(&pair->first);
            seen += *ValueGetBoolean(&pair->second) ? "=1 " : "=0 ";
            IteratorIncrement(&at);
        }
        IteratorDestruct(&at);
        IteratorDestruct(&end);
        Check(seen == "alpha=0 mid=1 zeta=1 ", "members are walked with their values");
        String key;
        StringConstructFrom(&key, "mid");
        Check(ValueReferValue(&root, &key) != nullptr, "referValue finds a member");
        StringDestruct(&key);
        StringConstructFrom(&key, "nope");
        Check(ValueReferValue(&root, &key) == nullptr, "and tells when there is none");
        StringDestruct(&key);
        ValueDestruct(&root);
    }

    // Not JSON: the value stays null and the call says so.
    {
        Value root;
        ValueConstruct(&root);
        Check(ParserParse(&root, "None", 4) != 0 && ValueGetType(&root) == 0 && !ValueToBool(&root),
              "\"None\" is refused");
        Check(ParserParse(&root, "", 0) != 0, "so is nothing");
        Check(ParserParse(&root, "[1, 2", 5) != 0 && ValueGetType(&root) == 0,
              "and a document that ends early");
        Check(ParserParse(&root, "true\0", 5) == 0 && *ValueGetBoolean(&root),
              "a terminator inside the size is not held against the text");
        ValueDestruct(&root);
    }

    // Building a document and writing it out.
    {
        Object object;
        ObjectConstruct(&object);
        String key;
        Value number;
        StringConstructFrom(&key, "speed");
        ValueConstructReal(&number, 1.5);
        ValueAssign(ObjectIndex(&object, &key), &number);
        ValueDestruct(&number);
        StringDestruct(&key);

        Array array;
        ArrayConstruct(&array);
        Value item;
        ValueConstructBool(&item, true);
        ArrayPushBack(&array, &item);
        ValueDestruct(&item);
        ValueConstructInteger(&item, -7);
        ArrayPushBack(&array, &item);
        ValueDestruct(&item);
        String word;
        StringConstructFrom(&word, "say \"hi\"");
        ValueConstructString(&item, &word);
        ArrayPushBack(&array, &item);
        ValueDestruct(&item);
        StringDestruct(&word);

        Value list;
        ValueConstructArray(&list, &array);
        ArrayDestruct(&array);
        StringConstructFrom(&key, "list");
        ValueAssign(ObjectIndex(&object, &key), &list);
        ValueDestruct(&list);
        StringDestruct(&key);

        Value document;
        ValueConstructObject(&document, &object);
        ObjectDestruct(&object);
        const std::string text = Serialized(document);
        Check(text == "{\"list\":[true,-7,\"say \\\"hi\\\"\"],\"speed\":1.5}", text.c_str());

        // And what was written reads back the same.
        Value again;
        ValueConstruct(&again);
        Check(ParserParse(&again, text.c_str(), text.size()) == 0 && Serialized(again) == text,
              "what is written parses to the same");
        ValueDestruct(&again);
        ValueDestruct(&document);
    }

    std::printf("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
