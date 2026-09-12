# Product tests

This directory is for executable or behavior-oriented validation: C++ netplay
tests, transport/relay harnesses, browser smoke tests, replay tests, and other
checks whose result is tied to observable program behavior.

Historical `*-feature-rebase-test.py` source-text checks are kept separately in
`audit/feature-rebase/`. They remain useful migration evidence, but they must
not be reported as product behavior coverage.

Static source-contract checks that are not tied to the historical feature rebase live in `audit/source-contracts/`.
