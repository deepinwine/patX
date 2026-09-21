import json
from pathlib import Path

from web_dossier.providers.epo_ops import (FamilyMember, _epodoc_publication,
                                           epodoc_to_docdb, parse_family,
                                           parse_number_service)

FIXTURES = Path(__file__).resolve().parent.parent / "fixtures" / "epo"


def test_publication_normalization():
    assert _epodoc_publication("CN119870049A") == "CN119870049A"
    assert _epodoc_publication("cn 119870049 a") == "CN119870049A"
    assert _epodoc_publication("US20240270309A1") == "US20240270309A1"
    assert _epodoc_publication("202410123457.5") == ""     # application, not publication
    assert epodoc_to_docdb("CN119870049A") == "CN.119870049.A"
    assert epodoc_to_docdb("US20240270309A1") == "US.20240270309.A1"


def test_number_service_parsing():
    obj = json.loads((FIXTURES / "number_service.json").read_text("utf-8"))
    apps = parse_number_service(obj)
    assert apps == ["CN202410123457A"]


def test_family_parsing():
    obj = json.loads((FIXTURES / "family_cn119870049.json").read_text("utf-8"))
    members = parse_family(obj)
    assert len(members) == 3
    by_country = {m.authority: m for m in members}
    assert by_country["CN"].application_number == "CN202410123457"
    assert by_country["CN"].publication_number == "CN119870049A"
    assert by_country["US"].application_number == "US18403211"
    assert by_country["US"].publication_number == "US20240270309A1"
    assert by_country["EP"].application_number == "EP24160000"
    assert by_country["CN"].application_date == "2024-01-19"


def test_provider_plumbing():
    from web_dossier.models import ResultCode
    from web_dossier.providers.epo_ops import EpoOpsProvider
    p = EpoOpsProvider("", "")
    assert not p.health_check()
    assert p.check_auth(None) == ResultCode.RESOLVE_FAILED
    out = p.list_documents("x", "y", None)
    assert out.code == ResultCode.UNSUPPORTED_JURISDICTION


def test_family_parsing_against_live_response():
    # Regression fixture captured from the real OPS gateway (EP1000000A1),
    # namespace-prefixed keys and all.
    obj = json.loads((FIXTURES / "family_ep1000000_real.json").read_text("utf-8"))
    members = parse_family(obj)
    assert len(members) == 6
    ep = [m for m in members if m.authority == "EP"]
    assert ep, "EP member missing"
    assert ep[0].application_number == "EP99203729"
    assert ep[0].application_date == "1999-11-08"
    assert ep[0].publication_number == "EP1000000A1"
