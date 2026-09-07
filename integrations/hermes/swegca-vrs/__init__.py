"""Standalone Hermes MemoryProvider; no patches to Hermes core required."""
from agent.memory_provider import MemoryProvider
from swegca_vrs_mcp.hermes_memory import HermesMemoryAdapter


class SWEGCAMemoryProvider(HermesMemoryAdapter, MemoryProvider):
    pass


def register(ctx):
    ctx.register_memory_provider(SWEGCAMemoryProvider())
