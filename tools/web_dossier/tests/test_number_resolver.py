from web_dossier.number_resolver import normalize_cn_identifier


def test_application_number_plain():
    ident = normalize_cn_identifier("202410123456.7")
    assert ident.valid and ident.number_type == "application"
    assert ident.normalized_number == "CN202410123456.7"


def test_application_number_cn_prefix_and_spaces():
    ident = normalize_cn_identifier("CN 2024 1012 3456.7")
    assert ident.number_type == "application"
    assert ident.normalized_number == "CN202410123456.7"


def test_publication_number_variants():
    for raw in ("CN119870049A", "cn119870049a", "CN 119870049 A", "119870049A"):
        ident = normalize_cn_identifier(raw)
        assert ident.number_type == "publication", raw
        assert ident.number == "119870049"
        assert ident.kind_code.upper() == "A"


def test_publication_is_not_application():
    # The classic bug the task warns about: stripping letters must NOT turn a
    # publication number into an application number.
    ident = normalize_cn_identifier("CN119870049A")
    assert ident.number_type == "publication"


def test_garbage_rejected():
    assert not normalize_cn_identifier("hello").valid
    assert not normalize_cn_identifier("").valid
    assert not normalize_cn_identifier("12345").valid
