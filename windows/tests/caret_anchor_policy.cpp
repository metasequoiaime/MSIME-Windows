#include "Key/CaretAnchorPolicy.h"

int main()
{
    // Do not use assert: Release builds must execute these checks too.
    if (KeyEventPayloadWriteMask(false) != 0b000111u)
        return 1;
    if (KeyEventPayloadWriteMask(true) != 0b001111u)
        return 2;
    if (!IsUsableCaretExtent(10, 20, 10, 40))
        return 3;
    if (IsUsableCaretExtent(10, 20, 10, 20))
        return 4;
    if (IsUsableCaretExtent(0, 0, 0, 0))
        return 5;
    if (IsUsableCaretExtent(12, 20, 10, 40))
        return 6;
    return 0;
}
