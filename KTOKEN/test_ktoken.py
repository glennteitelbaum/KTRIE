"""test_ktoken.py — pytest suite for ktoken module"""
import pytest
import sys, os
sys.path.insert(0, os.path.dirname(__file__))
import ktoken


UNICODE_DB = "UnicodeData.txt"
CL100K_VOCAB = "cl100k_base.tiktoken"


class TestCl100kTiktoken:
    @pytest.fixture
    def tok(self):
        return ktoken.tiktoken.Cl100k(UNICODE_DB, CL100K_VOCAB)

    def test_encode_str(self, tok):
        tokens = tok.encode("Hello, world!")
        assert isinstance(tokens, list)
        assert len(tokens) > 0
        assert all(isinstance(t, int) for t in tokens)

    def test_encode_bytes(self, tok):
        tokens = tok.encode(b"Hello, world!")
        assert len(tokens) > 0

    def test_decode(self, tok):
        tokens = tok.encode("Hello, world!")
        decoded = tok.decode(tokens)
        assert isinstance(decoded, bytes)
        assert decoded == b"Hello, world!"

    def test_decode_str(self, tok):
        tokens = tok.encode("Hello, world!")
        text = tok.decode_str(tokens)
        assert isinstance(text, str)
        assert text == "Hello, world!"

    def test_roundtrip(self, tok):
        texts = [
            "Hello, world!",
            "The quick brown fox jumps over the lazy dog.",
            "🎉 Unicode test: café, naïve, résumé",
            "  multiple   spaces   ",
            "",
            "a",
            "1234567890",
            "def foo(x):\n    return x + 1\n",
        ]
        for text in texts:
            tokens = tok.encode(text)
            result = tok.decode_str(tokens)
            assert result == text, f"Roundtrip failed for: {text!r}"

    def test_encode_batch(self, tok):
        texts = ["Hello", "World", "Test"]
        batch = tok.encode_batch(texts)
        assert len(batch) == 3
        for i, text in enumerate(texts):
            assert tok.decode_str(batch[i]) == text

    def test_vocab_size(self, tok):
        assert tok.vocab_size == 100256

    def test_repr(self, tok):
        r = repr(tok)
        assert "Cl100k" in r
        assert "100256" in r

    def test_encode_type_error(self, tok):
        with pytest.raises(TypeError):
            tok.encode(42)

    def test_empty_string(self, tok):
        tokens = tok.encode("")
        assert tokens == []

    def test_long_text(self, tok):
        text = "Hello world. " * 1000
        tokens = tok.encode(text)
        assert tok.decode_str(tokens) == text


class TestSubmodules:
    def test_tiktoken_exists(self):
        assert hasattr(ktoken, "tiktoken")

    def test_huggingface_exists(self):
        assert hasattr(ktoken, "huggingface")

    def test_tiktoken_classes(self):
        assert hasattr(ktoken.tiktoken, "Cl100k")
        assert hasattr(ktoken.tiktoken, "P50k")
        assert hasattr(ktoken.tiktoken, "O200k")

    def test_huggingface_classes(self):
        assert hasattr(ktoken.huggingface, "Cl100k")
        assert hasattr(ktoken.huggingface, "P50k")
        assert hasattr(ktoken.huggingface, "O200k")
