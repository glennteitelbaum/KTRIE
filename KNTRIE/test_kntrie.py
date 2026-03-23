"""test_kntrie.py — pytest suite for kntrie module"""
import pytest
import sys, os
sys.path.insert(0, os.path.dirname(__file__))
import kntrie


# ============================================================================
# Int64
# ============================================================================

class TestInt64:
    def test_basic_crud(self):
        m = kntrie.Int64()
        assert len(m) == 0
        assert not m

        m[10] = 100
        m[20] = 200
        m[5] = 50
        assert len(m) == 3
        assert m
        assert m[10] == 100
        assert m[20] == 200
        assert m[5] == 50

    def test_contains(self):
        m = kntrie.Int64()
        m[1] = 10
        assert 1 in m
        assert 2 not in m

    def test_get(self):
        m = kntrie.Int64()
        m[1] = 10
        assert m.get(1) == 10
        assert m.get(2) is None
        assert m.get(2, -1) == -1

    def test_getitem_missing(self):
        m = kntrie.Int64()
        with pytest.raises(KeyError):
            _ = m[99]

    def test_delitem(self):
        m = kntrie.Int64()
        m[1] = 10
        del m[1]
        assert 1 not in m
        assert len(m) == 0

    def test_delitem_missing(self):
        m = kntrie.Int64()
        with pytest.raises(KeyError):
            del m[99]

    def test_insert(self):
        m = kntrie.Int64()
        assert m.insert(1, 10)
        assert not m.insert(1, 20)
        assert m[1] == 10  # not overwritten

    def test_setitem_overwrites(self):
        m = kntrie.Int64()
        m[1] = 10
        m[1] = 20
        assert m[1] == 20


    def test_clear(self):
        m = kntrie.Int64()
        for i in range(100):
            m[i] = i
        assert len(m) == 100
        m.clear()
        assert len(m) == 0

    def test_memory_usage(self):
        m = kntrie.Int64()
        m[1] = 10
        assert m.memory_usage() > 0

    def test_iteration_ordered(self):
        m = kntrie.Int64()
        m[30] = 3
        m[10] = 1
        m[20] = 2
        items = list(m)
        assert items == [(10, 1), (20, 2), (30, 3)]

    def test_keys_values(self):
        m = kntrie.Int64()
        m[2] = 20
        m[1] = 10
        m[3] = 30
        assert m.keys() == [1, 2, 3]
        assert m.values() == [10, 20, 30]

    def test_items(self):
        m = kntrie.Int64()
        m[1] = 10
        m[2] = 20
        assert list(m.items()) == [(1, 10), (2, 20)]

    def test_negative_keys(self):
        m = kntrie.Int64()
        m[-5] = 50
        m[-1] = 10
        m[0] = 0
        m[3] = 30
        items = list(m)
        assert items == [(-5, 50), (-1, 10), (0, 0), (3, 30)]

    def test_repr(self):
        m = kntrie.Int64()
        m[1] = 10
        assert "Int64" in repr(m)
        assert "size=1" in repr(m)

    def test_bulk(self):
        m = kntrie.Int64()
        for i in range(10000):
            m[i] = i * 2
        assert len(m) == 10000
        assert m[5000] == 10000
        for i in range(0, 10000, 2):
            del m[i]
        assert len(m) == 5000


# ============================================================================
# Int32
# ============================================================================

class TestInt32:
    def test_basic(self):
        m = kntrie.Int32()
        m[1] = 42
        assert m[1] == 42

    def test_range(self):
        m = kntrie.Int32()
        m[1] = 2**31 - 1
        assert m[1] == 2**31 - 1


# ============================================================================
# Float
# ============================================================================

class TestFloat:
    def test_basic(self):
        m = kntrie.Float()
        m[1] = 3.14
        assert abs(m[1] - 3.14) < 1e-10


# ============================================================================
# Bool
# ============================================================================

class TestBool:
    def test_basic(self):
        m = kntrie.Bool()
        m[1] = True
        m[2] = False
        assert m[1] == True
        assert m[2] == False


# ============================================================================
# Object
# ============================================================================

class TestObject:
    def test_dict_value(self):
        m = kntrie.Object()
        m[1] = {"a": 1, "b": 2}
        assert m[1] == {"a": 1, "b": 2}

    def test_list_value(self):
        m = kntrie.Object()
        m[1] = [1, 2, 3]
        assert m[1] == [1, 2, 3]

    def test_setitem_list(self):
        m = kntrie.Object()
        m[1] = [1, 2, 3]
        m[1] = m[1] + [4]
        assert m[1] == [1, 2, 3, 4]

    def test_mixed_types(self):
        m = kntrie.Object()
        m[1] = "hello"
        m[2] = 42
        m[3] = [1, 2]
        m[4] = None
        assert m[1] == "hello"
        assert m[2] == 42
        assert m[3] == [1, 2]
        assert m[4] is None

    def test_conditional_erase(self):
        m = kntrie.Object()
        m[1] = [1, 2, 3]
        if 1 in m and len(m[1]) == 3:
            del m[1]
        assert 1 not in m
