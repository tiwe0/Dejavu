#include "hook_signature.h"

#include <cassert>
#include <string>

int main() {
    DejavuHookSignature signature;
    std::string error;

    assert(dejavu_parse_hook_signature("(ZBCSIJFDLjava/lang/String;[[I)V", &signature, &error));
    assert(signature.parameters.size() == 10);
    assert(signature.parameters[0] == 'Z');
    assert(signature.parameters[5] == 'J');
    assert(signature.parameters[6] == 'F');
    assert(signature.parameters[8] == 'L');
    assert(signature.parameters[9] == 'L');
    assert(signature.result == 'V');

    assert(dejavu_parse_hook_signature("()[Ljava/lang/Object;", &signature, &error));
    assert(signature.parameters.empty());
    assert(signature.result == 'L');

    assert(!dejavu_parse_hook_signature("I)I", &signature, &error));
    assert(!dejavu_parse_hook_signature("(V)V", &signature, &error));
    assert(!dejavu_parse_hook_signature("(Ljava/lang/String)V", &signature, &error));
    assert(!dejavu_parse_hook_signature("([V)V", &signature, &error));
    assert(!dejavu_parse_hook_signature("()VV", &signature, &error));
    return 0;
}
