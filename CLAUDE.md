# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

Machine-local notes are kept in `.claude/local-notes.md` (gitignored); read it if present before building, releasing, or changing the toolchain.

Project-specific agent guidance starts in **AGENTS.md**: it contains global/build rules and links to authoritative topic guides under `docs/agents/`. Read every guide relevant to the task, and edit the appropriate guide rather than duplicating its content here. Only `AGENTS.md` is imported below, so a fresh Claude Code instance loads the entry point without eagerly loading every topic guide:

@AGENTS.md
