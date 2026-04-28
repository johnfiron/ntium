#!/usr/bin/env python3
"""
Minimal cloud-agent batch orchestrator helper.

Purpose:
- Validate batch dependency readiness from cloud_agent_batches.json
- Emit launch payload templates for ready batches
- Keep implementation lightweight and dependency-free
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Dict, List, Set


def load_json(path: Path) -> dict:
    with path.open("r", encoding="utf-8") as f:
        return json.load(f)


def index_tickets(ticket_data: dict) -> Dict[str, dict]:
    tickets = ticket_data.get("tickets", [])
    return {t["id"]: t for t in tickets}


def gate_satisfied(gate: dict, ticket_index: Dict[str, dict]) -> bool:
    required = gate.get("required_tickets_done", [])
    for tid in required:
        ticket = ticket_index.get(tid)
        if not ticket or ticket.get("status") != "done":
            return False
    return True


def infer_completed_gates(batches: List[dict], ticket_index: Dict[str, dict]) -> Set[str]:
    completed = set()
    for b in batches:
        gate = b.get("gate", {})
        gate_name = gate.get("name")
        if gate_name and gate_satisfied(gate, ticket_index):
            completed.add(gate_name)
    return completed


def ready_batches(batches: List[dict], completed_gates: Set[str]) -> List[dict]:
    ready = []
    for b in batches:
        dep = b.get("depends_on_gate")
        if dep and dep not in completed_gates:
            continue
        gate = b.get("gate", {})
        # Skip already completed gates
        if gate.get("name") in completed_gates:
            continue
        ready.append(b)
    return ready


def make_launch_payload(batch: dict) -> dict:
    return {
        "batch_id": batch["id"],
        "description": batch.get("description", ""),
        "parallel_agents": batch.get("parallel_agents", []),
        "tickets": batch.get("tickets", []),
        "launch_template": "implementation-agent-launch",
        "notes": "Assign one agent per owner or epic branch. Commit/push per logical ticket.",
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="Cloud-agent batch orchestration helper")
    parser.add_argument(
        "--batches",
        default="project_management/automation/cloud_agent_batches.json",
        help="Path to cloud_agent_batches.json",
    )
    parser.add_argument(
        "--tickets",
        default="project_management/tickets/ticket_board.json",
        help="Path to ticket board json",
    )
    parser.add_argument(
        "--format",
        choices=["text", "json"],
        default="text",
        help="Output format",
    )
    args = parser.parse_args()

    batches_data = load_json(Path(args.batches))
    ticket_data = load_json(Path(args.tickets))

    batches = batches_data.get("batches", [])
    ticket_index = index_tickets(ticket_data)
    completed_gates = infer_completed_gates(batches, ticket_index)
    ready = ready_batches(batches, completed_gates)

    launch_payloads = [make_launch_payload(b) for b in ready]

    if args.format == "json":
        print(
            json.dumps(
                {
                    "completed_gates": sorted(completed_gates),
                    "ready_batches": launch_payloads,
                },
                indent=2,
            )
        )
        return 0

    print("Completed gates:")
    for g in sorted(completed_gates):
        print(f"- {g}")
    if not completed_gates:
        print("- (none)")
    print("\nReady batches:")
    if not launch_payloads:
        print("- (none)")
        return 0
    for p in launch_payloads:
        print(f"- {p['batch_id']}: agents={','.join(p['parallel_agents'])}")
        print(f"  tickets={','.join(p['tickets'])}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
