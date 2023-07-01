import drjit as dr
import types
import pytest
import re

array_types = []

for o1 in dr.__dict__.values():
    if isinstance(o1, types.ModuleType):
        for o2 in o1.__dict__.values():
            if isinstance(o2, type) and issubclass(o2, dr.ArrayBase):
                array_types.append(o2)


def test_arrays(*queries, name='t'):
    """
    Helper function used to parameterize testcases over Dr.Jit array types

    Takes a list of query strings that add or remove (if prefixed with '-')
    types based on matches against the ``__meta__`` descriptor.

    For example, ``@pytest.test_arrays("is_vector", "-bool")`` parameterizes
    the subsequent test function over vector-style arrays that don't have a
    boolean dtype.

    The type argument of the subsequent testcase must be named "t"
    """

    combined = set()
    if len(queries) == 0:
        combined = set(array_types)

    for query in queries:
        query = re.split(r',\s*(?![^()]*\))', query)
        result = set(array_types)
        for entry in query:
            if len(entry) == 0:
                continue

            remove = False
            if entry[0] == '-':
                entry = entry[1:]
                remove = True

            found = set(a for a in array_types if entry in a.__meta__)
            if remove:
                result = result.difference(found)
            else:
                result = result.intersection(found)

        combined |= result

    if len(combined) == 0:
        raise Exception('Query failed')

    ids = [a.__module__ + '.' + a.__name__ for a in combined]
    def wrapped(func):
        return pytest.mark.parametrize(name, combined, ids=ids)(func)

    return wrapped

@pytest.fixture(scope="module")
def drjit_verbose():
    level = dr.log_level()
    dr.set_log_level(100)
    yield
    dr.set_log_level(level)

def pytest_configure():
    pytest.test_arrays = test_arrays