# Atlas Agent (Python)

`atlas-agent` is a CLI that connects to a running Atlas instance over gRPC and lets you control it via an LLM-powered, multi-agent workflow.

## Requirements

- Python 3.12+
- Atlas must be running (open the app first) so it can listen on `localhost:50051`

## Installation

```bash
pip install atlas-agent
```

## Configuration

The agent requires an OpenAI-compatible API key:

- `OPENAI_API_KEY` (required)
- `OPENAI_BASE_URL` (optional) if you use a non-default endpoint (OpenAI-compatible providers)

Examples:

```bash
export OPENAI_API_KEY="..."
```

```bash
export OPENAI_API_KEY="..."
export OPENAI_BASE_URL="https://your-openai-compatible-endpoint/v1"
```

## Basic usage

Run the CLI (it starts an interactive chat session):

```bash
atlas-agent
```

Common options:

- `--model` (or `ATLAS_LLM_MODEL`) to choose the LLM model
- `--temperature` (or `ATLAS_LLM_TEMPERATURE`) to adjust response randomness

Help:

- Console: `atlas-agent --help`
- Module: `python -m atlas_agent --help`

## Development (monorepo)

If you are working inside the Atlas repo:

```bash
pip install -e python/atlas_agent
```

Or run from source by setting `PYTHONPATH` to include `python/atlas_agent/src`.
