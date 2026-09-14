"""tools/regtoken.py against a real database.

Skipped unless ``TRACKER_TEST_DB`` is set; see conftest.py.
"""

import importlib.util
import os
import sys

import pytest

from conftest import needs_db

pytestmark = needs_db

TOOLS = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                     'tools')


@pytest.fixture(scope='module')
def tool():
    """tools/regtoken.py loaded as a module, with tools/ importable as it is
    when the script runs."""
    if TOOLS not in sys.path:
        sys.path.insert(0, TOOLS)
    spec = importlib.util.spec_from_file_location(
        'regtoken_tool', os.path.join(TOOLS, 'regtoken.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_a_new_link_cancels_the_earlier_one(tool, database, monkeypatch, capsys):
    monkeypatch.setattr(sys, 'argv', ['regtoken.py', 'alice', 'tracker.example.com'])
    assert tool.main() == 0
    first = capsys.readouterr().out.strip()
    assert tool.main() == 0
    second = capsys.readouterr().out.strip()

    assert first != second
    tokens = [r['token'] for r in database.all(
        "SELECT `token` FROM `registration` WHERE `username` = 'alice'")]
    assert tokens == [second.rsplit('=', 1)[1]]
