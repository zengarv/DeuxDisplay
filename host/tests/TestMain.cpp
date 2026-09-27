#include "Check.h"

int main()
{
    RunProtocolTests();
    RunEdidTests();
    RunAnnexBTests();
    RunPointerShapeTests();
    RunRotationTests();
    RunTouchTests();
    RunAdbTests();

    if (g_failures != 0)
    {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("All tests passed\n");
    return 0;
}
