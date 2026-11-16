// SPDX-License-Identifier: GPL-3.0-or-later
// tests/fault_filter_test.cpp -- the first-chance handler's three rules.
//
// The console run at 954a97c did not die of a fault; it died of the handler
// logging DBG_PRINTEXCEPTION_C, which is what logging raises, until the stack was
// gone. These are the rules that make that impossible, tested where they cost a
// second instead of a boot.
#include <cstdint>

#include "harness.h"
#include "host/core/bbx_fault_filter.h"

using namespace bbx::core;

TEST(FaultFilter_AnOutputDebugStringPrintIsNeverLogged) {
    // 0x40010006 is what OutputDebugString raises. Logging it is what recursed.
    CHECK(DecideFault(0x40010006u, 0, false) == FaultAction::kPass);
    CHECK(DecideFault(0x4001000au, 0, false) == FaultAction::kPass);
    CHECK(DecideFault(0x80000003u, 0, false) == FaultAction::kPass);  // breakpoint
    CHECK(DecideFault(0x80000004u, 0, false) == FaultAction::kPass);  // single step
    CHECK(DecideFault(0xe06d7363u, 0, false) == FaultAction::kPass);  // a C++ throw
    CHECK(FaultCodeIsNoise(0x40010006u));
}

TEST(FaultFilter_ARealFaultIsLogged) {
    CHECK(DecideFault(0xc0000005u, 0, false) == FaultAction::kLog);   // access violation
    CHECK(DecideFault(0xc00000fdu, 0, false) == FaultAction::kLog);   // stack overflow
    CHECK(DecideFault(0x80000002u, 0, false) == FaultAction::kLog);   // data type misalignment
}

TEST(FaultFilter_AReenteredHandlerPassesTheFaultOnWithoutLogging) {
    // Even a real fault: if the guard is held, the handler is already writing, and
    // a second write is the recursion the console died of.
    CHECK(DecideFault(0xc0000005u, 0, true) == FaultAction::kPass);
    CHECK(DecideFault(0x40010006u, 3, true) == FaultAction::kPass);
}

TEST(FaultFilter_ThirtyTwoFaultsAndThenOneSuppressionLine) {
    CHECK(FaultGate::kMaxLogged == 32u);
    CHECK(DecideFault(0xc0000005u, 31, false) == FaultAction::kLog);
    CHECK(DecideFault(0xc0000005u, 32, false) == FaultAction::kSuppressed);
    CHECK(DecideFault(0xc0000005u, 33, false) == FaultAction::kSuppressed);
}

TEST(FaultFilter_TheGateAdmitsOneLinePerFaultUpToTheCap) {
    FaultGate gate;
    CHECK(gate.Enter());
    CHECK(!gate.Enter());  // the guard is held: the second call is not the owner
    gate.Leave();
    CHECK(gate.Enter());
    gate.Leave();
    std::uint32_t logged = 0, suppressed = 0, passed = 0;
    for (int i = 0; i < 100; ++i) {
        switch (gate.Admit(0xc0000005u)) {
        case FaultAction::kLog: ++logged; break;
        case FaultAction::kSuppressed: ++suppressed; break;
        case FaultAction::kPass: ++passed; break;
        }
    }
    CHECK_EQ(logged, 32u);
    CHECK_EQ(suppressed, 1u);  // the one line that says the rest were dropped
    CHECK_EQ(passed, 67u);
    CHECK_EQ(gate.logged(), 32u);
    // And the noise codes do not consume the budget.
    CHECK(gate.Admit(0x40010006u) == FaultAction::kPass);
    CHECK_EQ(gate.logged(), 32u);
}
