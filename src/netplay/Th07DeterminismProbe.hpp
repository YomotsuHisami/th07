#pragma once

namespace Netplay::Th07DeterminismProbe
{
// Called immediately around exactly one canonical fixed-60-Hz simulation tick.
// Ordinary builds are transparent unless debugHarness=netplay-dual-stage1.
void BeforeSimulationTick();
void AfterSimulationTick(int chainResult);
} // namespace Netplay::Th07DeterminismProbe
