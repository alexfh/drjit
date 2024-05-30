import drjit as dr
from dataclasses import dataclass
import pytest
import re

@pytest.test_arrays('jit,-tensor')
@pytest.mark.parametrize('eval', [True, False])
def test01_simple(t, eval):
    if dr.size_v(t) == 0:
        with pytest.raises(TypeError, match="type does not contain any Jit-tracked arrays"):
            dr.alloc_local(t, 10)
        return

    if dr.size_v(t) == dr.Dynamic:
        s = dr.alloc_local(t, 10, dr.zeros(t, 3))
    else:
        s = dr.alloc_local(t, 10, dr.zeros(t))
    v = s[0]
    assert len(s) == 10
    assert v.state == dr.VarState.Literal
    assert dr.all(v == t(0), axis=None)

    s[0] = t(1)
    if eval:
        dr.eval(s)

    assert dr.all((s[0] == t(1)) & (s[1] == t(0)), axis=None)


@pytest.test_arrays('jit,uint32,shape=(*)')
@dr.syntax
def test02_fill_in_loop_then_read(t):
    s = dr.alloc_local(t, 10)
    i = t(0)

    while i < 10:
        i += 1
        s[i] = t(i)

    assert s[3] == 3


@pytest.test_arrays('jit,uint32,shape=(*)')
@pytest.mark.parametrize('variant', [0,1])
@dr.syntax
def test03_bubble_sort(t, variant):
    import sys
    n = 32
    s = dr.alloc_local(t, n)
    rng = sys.modules[t.__module__].PCG32(10000)
    Bool = dr.mask_t(t)

    i = t(0)
    while i < n:
        s[i] = rng.next_uint32()
        i += 1


    i = t(0)
    cont=Bool(True)
    while (i < n-1) & cont:
        j = t(0)
        cont=Bool(variant==0)
        while j < n-i-1:
            if dr.hint(variant == 0, mode='scalar'):
                s0, s1 = s[j], s[j+1]
                if s0 > s1:
                    s0, s1 = s1, s0
                s[j], s[j+1] = s0, s1
            else:
                if s[j] > s[j+1]:
                    s[j], s[j+1] = s[j+1], s[j]
                    cont = Bool(True)
            j+= 1
        i += 1

    result = [s[j] for j in range(n)]
    dr.eval(result)
    for i in range(len(result)-1):
        assert dr.all(result[i] <= result[i + 1])


@pytest.test_arrays('jit,uint32,shape=(*)')
@dr.syntax
def test04_conditional(t):
    s = dr.alloc_local(t, 1, value = dr.zeros(t, 2))
    i = t(0, 1)

    if i > 0:
        s[0] = 10
    else:
        s[0] = 11

    for i in range(2): # evaluate twice (intentional)
        assert dr.all(s[0] == [11, 10])


@pytest.test_arrays('diff,float32,shape=(*)')
@dr.syntax
def test05_nodiff(t):
    s = dr.alloc_local(t, 1)
    x = t(0)
    dr.enable_grad(x)
    with pytest.raises(RuntimeError, match=re.escape(r"Local memory writes are not differentiable. You must use 'drjit.detach()' to disable gradient tracking of the written value.")):
        s[0] = x


@pytest.test_arrays('jit,-diff,float32,shape=(*)')
@dr.syntax
def test06_copy(t):
    s0 = dr.alloc_local(t, 2)
    s0[0] = 123
    s0[1] = 456

    s1 = dr.Local(s0)
    s1[0] += 100
    s0[0] += 1000


    assert s0[0] == 1123
    assert s0[1] == 456
    assert s1[0] == 223
    assert s1[1] == 456


@pytest.test_arrays('jit,-diff,float32,shape=(*)')
@dr.syntax
def test07_pytree(t):
    from dataclasses import dataclass

    @dataclass
    class XY:
        x: t
        y: t

    result = dr.alloc_local(XY, size=2, value=dr.zeros(XY))
    result[0] = XY(t(3),t(4))
    result[1] = XY(t(5),t(6))
    assert "XY(x=[3], y=[4])" in str(result[0])
    assert "XY(x=[5], y=[6])" in str(result[1])


@pytest.test_arrays('jit,-diff,uint32,shape=(*)')
@dr.syntax
def test08_oob_read(t, capsys):
    i, r = t(0), t(0)
    v = dr.alloc_local(t, size=10, value=t(0))
    with pytest.raises(RuntimeError, match=r"out of bounds read \(source size=10, offset=100\)"):
        v[100]
    with pytest.raises(RuntimeError, match=r"out of bounds write \(target size=10, offset=100\)"):
        v[100] = 0
    with dr.scoped_set_flag(dr.JitFlag.Debug, True):
        while i < 100:
            r += v[i]
            i += 1
        assert r == 0

    transcript = capsys.readouterr().err
    assert 'drjit.Local.read(): out-of-bounds read from position 99 in an array of size 10' in transcript


@pytest.test_arrays('jit,-diff,uint32,shape=(*)')
@dr.syntax
def test09_oob_write(t, capsys):
    i, r = t(0), t(0)
    v = dr.alloc_local(t, size=10, value=t(0))
    with dr.scoped_set_flag(dr.JitFlag.Debug, True):
        while i < 100:
            v[i] = i
            i += 1
    print(v[0])

    transcript = capsys.readouterr().err
    assert 'drjit.Local.write(): out-of-bounds write to position 99 in an array of size 10' in transcript
