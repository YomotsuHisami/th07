# Source-contract audits

These scripts inspect TH06/TH07/Host source text to guard architectural and
protocol ownership assumptions. They are static audits, not runtime behavior
tests.

A PASS here cannot prove peer recovery, Cherry behavior, localization texture
allocation, spectator backlog handling, or spectator lifecycle behavior. Those
claims require executable C++ tests, relay/server harnesses, browser smoke
tests, and real build/runtime validation.

Keep source-string checks here rather than under `tests/`. Avoid assertions on
comments, whitespace, incidental statement formatting, or historical hunk
counts unless the file is explicitly a historical migration audit.
