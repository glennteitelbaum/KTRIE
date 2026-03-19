"""test_kstrie.py — pytest suite for kstrie module"""
import pytest
import sys, os
sys.path.insert(0, os.path.dirname(__file__))
import kstrie


# ============================================================================
# Int64
# ============================================================================

class TestInt64:
    def test_basic_crud(self):
        m = kstrie.Int64()
        assert len(m) == 0
        assert not m

        m["hello"] = 1
        m["world"] = 2
        assert len(m) == 2
        assert m
        assert m["hello"] == 1

    def test_contains(self):
        m = kstrie.Int64()
        m["abc"] = 1
        assert "abc" in m
        assert "xyz" not in m

    def test_get(self):
        m = kstrie.Int64()
        m["k"] = 10
        assert m.get("k") == 10
        assert m.get("missing") is None
        assert m.get("missing", -1) == -1

    def test_getitem_missing(self):
        m = kstrie.Int64()
        with pytest.raises(KeyError):
            _ = m["nope"]

    def test_delitem(self):
        m = kstrie.Int64()
        m["x"] = 1
        del m["x"]
        assert "x" not in m

    def test_delitem_missing(self):
        m = kstrie.Int64()
        with pytest.raises(KeyError):
            del m["nope"]

    def test_insert(self):
        m = kstrie.Int64()
        assert m.insert("a", 1)
        assert not m.insert("a", 2)
        assert m["a"] == 1

    def test_setitem_overwrites(self):
        m = kstrie.Int64()
        m["a"] = 1
        m["a"] = 2
        assert m["a"] == 2

    def test_modify(self):
        m = kstrie.Int64()
        m["k"] = 10
        assert m.modify("k", lambda v: v + 5)
        assert m["k"] == 15
        assert not m.modify("miss", lambda v: v)

    def test_modify_with_default(self):
        m = kstrie.Int64()
        assert not m.modify("k", lambda v: v * 2, 42)
        assert m["k"] == 42
        assert m.modify("k", lambda v: v * 2, 0)
        assert m["k"] == 84

    def test_erase_when(self):
        m = kstrie.Int64()
        m["a"] = 10
        assert not m.erase_when("a", lambda v: v == 99)
        assert "a" in m
        assert m.erase_when("a", lambda v: v == 10)
        assert "a" not in m

    def test_clear(self):
        m = kstrie.Int64()
        for i in range(50):
            m[f"key_{i}"] = i
        m.clear()
        assert len(m) == 0

    def test_iteration_ordered(self):
        m = kstrie.Int64()
        m["banana"] = 1
        m["apple"] = 2
        m["cherry"] = 3
        keys = [k for k, v in m]
        assert keys == ["apple", "banana", "cherry"]

    def test_keys_values(self):
        m = kstrie.Int64()
        m["b"] = 2
        m["a"] = 1
        assert m.keys() == ["a", "b"]
        assert m.values() == [1, 2]

    def test_empty_key(self):
        m = kstrie.Int64()
        m[""] = 99
        assert m[""] == 99
        assert "" in m
        assert len(m) == 1

    def test_memory_usage(self):
        m = kstrie.Int64()
        m["test"] = 1
        assert m.memory_usage() > 0

    def test_repr(self):
        m = kstrie.Int64()
        m["x"] = 1
        assert "Int64" in repr(m)

    def test_bulk(self):
        m = kstrie.Int64()
        for i in range(5000):
            m[f"item_{i:05d}"] = i
        assert len(m) == 5000
        assert m["item_02500"] == 2500


# ============================================================================
# Prefix operations
# ============================================================================

class TestPrefix:
    @pytest.fixture
    def trie(self):
        m = kstrie.Int64()
        m["apple"] = 1
        m["app"] = 2
        m["application"] = 3
        m["banana"] = 4
        m["band"] = 5
        m["ban"] = 6
        return m

    def test_prefix_count(self, trie):
        assert trie.prefix_count("app") == 3
        assert trie.prefix_count("ban") == 3
        assert trie.prefix_count("x") == 0
        assert trie.prefix_count("") == 6
        assert trie.prefix_count("apple") == 1
        assert trie.prefix_count("apples") == 0
        assert trie.prefix_count("b") == 3
        assert trie.prefix_count("a") == 3

    def test_prefix_items(self, trie):
        items = trie.prefix_items("app")
        assert len(items) == 3
        assert items[0] == ("app", 2)
        assert items[1] == ("apple", 1)
        assert items[2] == ("application", 3)

    def test_prefix_keys(self, trie):
        keys = trie.prefix_keys("app")
        assert keys == ["app", "apple", "application"]

    def test_prefix_values(self, trie):
        vals = trie.prefix_values("ban")
        assert len(vals) == 3
        assert set(vals) == {4, 5, 6}

    def test_prefix_items_empty(self, trie):
        assert trie.prefix_items("xyz") == []

    def test_prefix_items_all(self, trie):
        assert len(trie.prefix_items("")) == 6

    def test_prefix_copy(self, trie):
        sub = trie.prefix_copy("ban")
        assert len(sub) == 3
        assert len(trie) == 6  # source unchanged
        assert sub["banana"] == 4
        assert sub["band"] == 5
        assert sub["ban"] == 6
        with pytest.raises(KeyError):
            _ = sub["apple"]

    def test_prefix_erase(self, trie):
        n = trie.prefix_erase("ban")
        assert n == 3
        assert len(trie) == 3
        assert "banana" not in trie
        assert "apple" in trie

    def test_prefix_erase_no_match(self, trie):
        assert trie.prefix_erase("xyz") == 0
        assert len(trie) == 6

    def test_prefix_erase_all(self, trie):
        n = trie.prefix_erase("")
        assert n == 6
        assert len(trie) == 0

    def test_prefix_split(self, trie):
        split = trie.prefix_split("app")
        assert len(split) == 3
        assert len(trie) == 3
        assert split["apple"] == 1
        assert split["app"] == 2
        assert split["application"] == 3
        assert "apple" not in trie
        assert "banana" in trie

    def test_prefix_split_no_match(self, trie):
        split = trie.prefix_split("xyz")
        assert len(split) == 0
        assert len(trie) == 6

    def test_prefix_split_all(self, trie):
        split = trie.prefix_split("")
        assert len(split) == 6
        assert len(trie) == 0

    def test_prefix_copy_independent(self, trie):
        sub = trie.prefix_copy("app")
        trie.prefix_erase("app")
        # sub still has its data
        assert len(sub) == 3
        assert sub["apple"] == 1

    def test_prefix_large(self):
        m = kstrie.Int64()
        for i in range(1000):
            m[f"item/{i // 100}/{i}"] = i
        assert m.prefix_count("item/3/") == 100

        sub = m.prefix_copy("item/5/")
        assert len(sub) == 100
        assert len(m) == 1000

        n = m.prefix_erase("item/5/")
        assert n == 100
        assert len(m) == 900

        split = m.prefix_split("item/3/")
        assert len(split) == 100
        assert len(m) == 800

    def test_prefix_compact_filtered(self):
        """Prefix lands in a compact leaf with partial match."""
        m = kstrie.Int64()
        m["abc"] = 1
        m["abd"] = 2
        m["xyz"] = 3
        assert m.prefix_count("ab") == 2
        assert m.prefix_keys("ab") == ["abc", "abd"]

        n = m.prefix_erase("ab")
        assert n == 2
        assert len(m) == 1
        assert m["xyz"] == 3

    def test_prefix_exact_key_with_children(self):
        m = kstrie.Int64()
        m["ab"] = 10
        m["abc"] = 20
        assert m.prefix_count("ab") == 2
        items = m.prefix_items("ab")
        assert len(items) == 2

    def test_prefix_on_empty(self):
        m = kstrie.Int64()
        assert m.prefix_count("x") == 0
        assert m.prefix_items("x") == []
        assert m.prefix_erase("x") == 0
        split = m.prefix_split("x")
        assert len(split) == 0


# ============================================================================
# Other value types
# ============================================================================

class TestFloat:
    def test_basic(self):
        m = kstrie.Float()
        m["pi"] = 3.14
        assert abs(m["pi"] - 3.14) < 1e-10

    def test_prefix(self):
        m = kstrie.Float()
        m["a/1"] = 1.0
        m["a/2"] = 2.0
        m["b/1"] = 3.0
        assert m.prefix_count("a/") == 2


class TestBool:
    def test_basic(self):
        m = kstrie.Bool()
        m["yes"] = True
        m["no"] = False
        assert m["yes"] == True
        assert m["no"] == False


class TestObject:
    def test_dict_value(self):
        m = kstrie.Object()
        m["user/1"] = {"name": "alice"}
        assert m["user/1"] == {"name": "alice"}

    def test_modify(self):
        m = kstrie.Object()
        m["k"] = [1, 2]
        m.modify("k", lambda v: v + [3])
        assert m["k"] == [1, 2, 3]

    def test_prefix_copy(self):
        m = kstrie.Object()
        m["a/1"] = {"x": 1}
        m["a/2"] = {"x": 2}
        m["b/1"] = {"x": 3}
        sub = m.prefix_copy("a/")
        assert len(sub) == 2
        assert sub["a/1"] == {"x": 1}
