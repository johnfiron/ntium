# Project Management Pack

This folder contains an import-ready execution package for the Windows 11 25H2
live wallpaper project.

## Contents

- `tickets/ticket_board.csv`:
  Spreadsheet import format for issue trackers.
- `tickets/ticket_board.json`:
  Structured board payload for automation pipelines.
- `agents/agent_registry.json`:
  Agent roster, ownership, and deliverables.
- `agents/prompts.md`:
  Ready-to-use prompts for each agent role.
- `automation/execution_dag.mmd`:
  Mermaid DAG of technical dependencies.
- `automation/cloud_agent_batches.json`:
  Parallel launch batches with dependency gates.
- `automation/runbook.md`:
  Operator guide for running cloud agents and merges.
- `automation/orchestrator.py`:
  Dependency-aware helper that reports ready batches and emits launch payloads.

## Required Ticket Fields

Every ticket record includes:

- `id`
- `owner`
- `dependencies`
- `acceptance_criteria`
- `test_cases`
- `risk_tag`

Additional optional fields are included to help prioritization and reporting.

## Import Notes

1. Import `ticket_board.csv` directly into issue boards that support CSV.
2. Use `ticket_board.json` for API ingestion and dashboard generation.
3. Track progress by status values:
   - `todo`
   - `in_progress`
   - `blocked`
   - `done`
