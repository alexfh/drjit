import ast
import types
import inspect
from typing import Callable, Optional, List


class SyntaxVisitor(ast.NodeTransformer):
    def __init__(self):
        super().__init__()

        # Keep track of read/written variables
        self.var_r, self.var_w = set(), set()

        # As the above, but for parent AST nodes
        self.par_r, self.par_w = [], []

        # Enable the syntax visitor transformations
        self.enabled = True

    def visit_FunctionDef(self, node: ast.FunctionDef):
        if self.enabled:
            # Process only the outermost function
            self.enabled = False

            # Add function parameters to self.var_w
            for o1 in (node.args.args, node.args.posonlyargs, node.args.kwonlyargs):
                for o2 in o1:
                    self.var_w.add(o2.arg)

            node = self.generic_visit(node)

        return node

    def visit_Name(self, node: ast.Name):
        if isinstance(node.ctx, ast.Load):
            self.var_r.add(node.id)
        elif isinstance(node.ctx, ast.Store):
            self.var_w.add(node.id)
        return node

    def extract_hints(self, node: ast.AST):
        if (
            not isinstance(node, ast.Call)
            or not isinstance(node.func, ast.Attribute)
            or node.func.attr != "hint"
        ):
            return node, {}

        if len(node.args) != 1:
            raise RuntimeError("drjit.hint(): must have a single positional argument.")

        hints = {}
        for k in node.keywords:
            hints[k.arg] = k.value

        if "exclude" in hints:
            exclude = set()
            if isinstance(k.value, ast.List):
                for e in k.value.elts:
                    if isinstance(e, ast.Name):
                        exclude.add(e.id)
                    else:
                        exclude = None
                        break
            else:
                exclude = None

            if exclude is None:
                raise Exception(
                    "dr.hint(): The 'exclude' parameter must specify "
                    "a literal list of strings (e.g., ['a', 'b'])."
                )

            hints["exclude"] = exclude

        valid_keys = ["exclude", "label", "method", "max_iterations"]
        for k in hints.keys():
            if k not in valid_keys:
                raise RuntimeError(f'drjit.hint(): unsupported keyword argument "{k}".')
        return node.args[0], hints

    def rewrite_and_track(self, node: ast.AST):
        # Keep track of variable reads/writes
        self.par_r.append(self.var_r)
        self.par_w.append(self.var_w)
        self.var_r = set()
        self.var_w = set()

        # Process the node recursively
        node = self.generic_visit(node)

        # Extract hints, if available
        node.test, hints = self.extract_hints(node.test)

        # Collect read/written variables
        var_r, var_w = self.var_r, self.var_w
        par_r, par_w = set(), set()
        for s in self.par_r:
            par_r |= s
        for s in self.par_w:
            par_w |= s

        # 3. Compute data mapping:
        # 3a. Don't import globals into state data structure
        temp = set()
        for k in var_r:
            if k not in var_w and k not in par_w:
                temp.add(k)
        var_r -= temp

        # 3b. Don't store loop temporaries. We consider temporaries
        # to be variables that haven't been defined before the loop
        # (which would be undefined if the condition is False)
        temp = set()
        for k in var_w:
            if k not in par_w:
                temp.add(k)
        var_r -= temp
        var_w -= temp

        # 3c: exclude variables as requested by the user
        if "exclude" in hints:
            exclude = hints["exclude"]
            var_r -= set(exclude)
            var_w -= set(exclude)

        self.var_r = var_r | self.par_r.pop()
        self.var_w = var_w | self.par_w.pop()

        state = sorted(var_r | var_w)

        method = hints.get("method", None)
        is_scalar = isinstance(method, ast.Constant) and method.value == "scalar"

        return node, state, hints, is_scalar

    def visit_If(self, node: ast.If):
        (node, state, hints, is_scalar) = self.rewrite_and_track(node)

        if is_scalar:
            return node

        # 1. Names of generated functions
        ifstmt_name = "_if_stmt"
        cond_name = ifstmt_name + "_cond"
        true_name = ifstmt_name + "_true"
        false_name = ifstmt_name + "_false"

        # 2. Generate a function representing the condition
        #    .. which takes all state variables as input
        func_args = ast.arguments(
            args=[ast.arg(k) for k in state],
            posonlyargs=[],
            kwonlyargs=[],
            defaults=[],
            kw_defaults=[],
        )

        cond_func = ast.FunctionDef(
            name=cond_name,
            args=func_args,
            body=[ast.Return(value=node.test)],
            decorator_list=[],
            lineno=node.lineno,
            col_offset=node.col_offset,
            end_lineno=node.end_lineno,
            end_col_offset=node.end_col_offset,
        )

        # 3. Generate a function representing the if/else branches
        load, store, delete = ast.Load(), ast.Store(), ast.Del()
        true_fn = ast.FunctionDef(
            name=true_name,
            args=func_args,
            body=[
                *node.body,
                ast.Return(
                    value=ast.Tuple(
                        elts=[ast.Name(id=k, ctx=load) for k in state], ctx=load
                    )
                ),
            ],
            decorator_list=[],
            lineno=node.lineno,
            col_offset=node.col_offset,
            end_lineno=node.end_lineno,
            end_col_offset=node.end_col_offset,
        )

        false_fn = ast.FunctionDef(
            name=false_name,
            args=func_args,
            body=[
                *node.orelse,
                ast.Return(
                    value=ast.Tuple(
                        elts=[ast.Name(id=k, ctx=load) for k in state], ctx=load
                    )
                ),
            ],
            decorator_list=[],
            lineno=node.lineno,
            col_offset=node.col_offset,
            end_lineno=node.end_lineno,
            end_col_offset=node.end_col_offset,
        )

        # 7. Import the Dr.Jit if_stmt function
        import_stmt = ast.ImportFrom(
            module="drjit",
            names=[ast.alias(name="if_stmt", asname=ifstmt_name)],
            level=0,
        )

        # 8. Call drjit.if_stmt()
        call_kwargs = [
            ast.keyword(
                arg="state_labels",
                value=ast.Tuple(
                    elts=[ast.Constant(k) for k in state],
                    ctx=load,
                ),
            ),
        ]

        for k, v in hints.items():
            if k == "exclude":
                continue
            call_kwargs.append(ast.keyword(arg=k, value=v))

        if_expr = ast.Assign(
            targets=[
                ast.Tuple(
                    elts=[ast.Name(id=k, ctx=store) for k in state],
                    ctx=store,
                )
            ],
            value=ast.Call(
                func=ast.Name(id=ifstmt_name, ctx=load),
                args=[
                    ast.Tuple(elts=[ast.Name(id=k, ctx=load) for k in state], ctx=load),
                    ast.Name(id=cond_name, ctx=load),
                    ast.Name(id=true_name, ctx=load),
                    ast.Name(id=false_name, ctx=load),
                ],
                keywords=call_kwargs,
                lineno=node.lineno,
                col_offset=node.col_offset,
                end_lineno=node.end_lineno,
                end_col_offset=node.end_col_offset,
            ),
        )

        # 10. Some comments (as strings) to delineate processed parts of the AST
        comment_start = ast.Expr(
            ast.Constant("---- if statement transformed by dr.syntax ----")
        )
        comment_mid = ast.Expr(
            ast.Constant("------------- invoke dr.if_stmt ---------------")
        )
        comment_end = ast.Expr(
            ast.Constant("-----------------------------------------------")
        )

        # 10. Delete local variables created while processing the loop
        cleanup_targets = [
            ast.Name(id=ifstmt_name, ctx=delete),
            ast.Name(id=cond_name, ctx=delete),
            ast.Name(id=true_name, ctx=delete),
            ast.Name(id=false_name, ctx=delete),
        ]

        cleanup = ast.Delete(targets=cleanup_targets)

        return (
            comment_start,
            cond_func,
            true_fn,
            false_fn,
            comment_mid,
            import_stmt,
            if_expr,
            cleanup,
            comment_end,
        )

    def visit_While(self, node: ast.While):
        (node, state, hints, is_scalar) = self.rewrite_and_track(node)
        if is_scalar:
            return node

        # 1. Names of generated functions
        loop_name = "_loop"
        cond_name = loop_name + "_cond"
        body_name = loop_name + "_body"

        # 5. Generate a function representing the loop condition
        #    .. which takes all loop state variables as input
        func_args = ast.arguments(
            args=[ast.arg(k) for k in state],
            posonlyargs=[],
            kwonlyargs=[],
            defaults=[],
            kw_defaults=[],
        )

        cond_func = ast.FunctionDef(
            name=cond_name,
            args=func_args,
            body=[ast.Return(value=node.test)],
            decorator_list=[],
            lineno=node.lineno,
            col_offset=node.col_offset,
            end_lineno=node.end_lineno,
            end_col_offset=node.end_col_offset,
        )

        # 6. Generate a function representing the loop body
        load, store, delete = ast.Load(), ast.Store(), ast.Del()
        body_func = ast.FunctionDef(
            name=body_name,
            args=func_args,
            body=[
                *node.body,
                ast.Return(
                    value=ast.Tuple(
                        elts=[ast.Name(id=k, ctx=load) for k in state], ctx=load
                    )
                ),
            ],
            decorator_list=[],
            lineno=node.lineno,
            col_offset=node.col_offset,
            end_lineno=node.end_lineno,
            end_col_offset=node.end_col_offset,
        )

        # 7. Import the Dr.Jit while_loop function
        import_stmt = ast.ImportFrom(
            module="drjit",
            names=[ast.alias(name="while_loop", asname=loop_name)],
            level=0,
        )

        # 8. Call drjit.while_loop()
        call_kwargs = [
            ast.keyword(
                arg="state_labels",
                value=ast.Tuple(
                    elts=[ast.Constant(k) for k in state],
                    ctx=load,
                ),
            ),
        ]
        for k, v in hints.items():
            if k == "exclude":
                continue
            call_kwargs.append(ast.keyword(arg=k, value=v))

        while_expr = ast.Assign(
            targets=[
                ast.Tuple(
                    elts=[ast.Name(id=k, ctx=store) for k in state],
                    ctx=store,
                )
            ],
            value=ast.Call(
                func=ast.Name(id=loop_name, ctx=load),
                args=[
                    ast.Tuple(elts=[ast.Name(id=k, ctx=load) for k in state], ctx=load),
                    ast.Name(id=cond_name, ctx=load),
                    ast.Name(id=body_name, ctx=load),
                ],
                keywords=call_kwargs,
                lineno=node.lineno,
                col_offset=node.col_offset,
                end_lineno=node.end_lineno,
                end_col_offset=node.end_col_offset,
            ),
        )

        # 9. Some comments (as strings) to delineate processed parts of the AST
        comment_start = ast.Expr(
            ast.Constant("-------- loop transformed by dr.syntax --------")
        )
        comment_mid = ast.Expr(
            ast.Constant("----------- invoke dr.while_loop --------------")
        )
        comment_end = ast.Expr(
            ast.Constant("-----------------------------------------------")
        )

        # 10. Delete local variables created while processing the loop
        cleanup_targets = [
            ast.Name(id=loop_name, ctx=delete),
            ast.Name(id=cond_name, ctx=delete),
            ast.Name(id=body_name, ctx=delete),
        ]

        cleanup = ast.Delete(targets=cleanup_targets)

        return (
            comment_start,
            cond_func,
            body_func,
            comment_mid,
            import_stmt,
            while_expr,
            cleanup,
            comment_end,
        )


def syntax(f: Callable = None, print_ast: bool = False, print_code: bool = False):
    """
    Syntax decorator for vectorized loops and conditionals.

    This decorator provides *syntax sugar*. It allows users to write natural
    Python code that it then turns into native Dr.Jit constructs. It *does not
    compile* or otherwise change the behavior of the function.

    The :py:func:`@drjit.syntax <drjit.syntax>` decorator introduces two
    specific changes:

    1. It rewrites ``while`` loops so that they still work when the loop
       condition is a Dr.Jit array. In that case, each element of the array
       may want to run a different number of loop iterations.

    2. Analogously, it rewrites ``if`` statements so that they work when the
       conditional expression is a Dr.Jit array. In that case, only a subset of
       array elements may want to execute the body of the ``if`` statement.

    Other control flow statements are unaffected. The transformed function may
    call other functions, whether annotated by :py:func:`drjit.syntax` or
    not. The introduced transformations only affect the annotated function.

    Internally, function turns ``while`` loops and ``if`` statements into calls
    to :py:func:`drjit.while_loop` and :py:func:`drjit.if_stmt`. It is tedious
    to write large programs in this way, which is why the decorator exists.

    For example, consider the following function that raises a floating point
    array to an integer power.

    .. code-block:: python

       import drjit as dr
       from drjit.cuda import Int, Float

       @dr.syntax
       def ipow(x: Float, n: Int):
           result = Float(1)

           while n != 0:
               if n & 1 != 0:
                   result *= x
               x *= x
               n >>= 1

           return result

    Note that this function is *vectorized*: its inputs (of types
    :py:class:`drjit.cuda.Int` and :py:class:`drjit.cuda.Float`) represent
    dynamic arrays that could contain large numbers of elements.

    The resulting code looks natural thanks to the :py:func:`@drjit.syntax
    <drjit.syntax>` decorator. Following application of this decorator, the
    function (roughly) expands into the following native Python code that
    determines relevant state variables and wraps conditionals and blocks into
    functions passed to :py:func:`drjit.while_loop` and
    :py:func:`drjit.if_stmt`. These transformations enable Dr.Jit to
    symbolically compile and automatically differentiate the implementation in
    both forward and reverse modes (if desired).

    .. code-block:: python

       def ipow(x: Float, n: Int):
           # Loop condition wrapped into a callable for ``drjit.while_loop``
           def loop_cond(n, x, result):
               return n != 0

           # Loop body wrapped into a callable for ``drjit.while_loop``
           def loop_body(n, x, result):
               # Conditional expression wrapped into callable for drjit.if_stmt
               def if_cond(n, x, result):
                   return n & 1 != 0

               # Conditional body wrapped into callable for drjit.if_stmt
               def if_body(n, x, result):
                   result *= x

                   # Return updated state following conditional stmt
                   return (n, x, result)

               # Map the 'n', 'x', and 'result' variables though the conditional
               n, x, result = dr.if_stmt(
                   (n, x, result),
                   if_cond,
                   if_body
               )

               # Rest of the loop body copy-pasted (no transformations needed here)
               x *= x
               n >>= 1

               # Return updated loop state
               return (n, x, result)

           result = Float(1)

           # Execute the loop and assign the final loop state to local variables
           n, x, result = dr.while_loop(
               (n, x, result)
               loop_cond,
               loop_body
           )

           return result

    The :py:func:`@drjit.syntax <drjit.syntax>` decorator runs *once* when
    the function is first defined. Calling the resulting function does not
    involve additional transformation steps. The transformation preserves line
    number information so that debugging works and exeptions/error messages are
    tied to the right locations in the corresponding *untransformed* function.

    Note that this decorator can only be used when the code to be transformed
    is part of a function. It cannot be applied to top-level statements on the
    Python REPL, or in a Jupyter notebook cell (unless that cell defines a
    function and applies the decorator to it).

    The two optional keyword arguments ``print_ast`` and ``print_code`` are
    both disabled by default. Set them to ``True`` to inspect the function
    before/after the transformation, either using an AST dump or via generated
    Python code

    .. code-block:: python

       @dr.syntax(print_code=True)
       def ipow(x: Float, n: Int):
           # ...

    (This feature is mostly relevant for developers working on Dr.Jit
    internals).

    Note that the functions :py:func:`if_stmt` and :py:func:`while_loop` even
    work when the loop condition is *scalar* (a Python `bool`). Since they
    don't do anything special in that case and may add (very) small overheads,
    you may want to avoid the transformation altogether. You can provide such
    control flow hints using :py:func:`drjit.hint`. Other hints can also be
    provided to request compilation using evaluated/symbolic mode, or to
    specify a maximum number of loop iteration for reverse-mode automatic
    differentiation.

    .. code-block:: python

       @dr.syntax
       def foo():
           i = 0 # 'i' is a Python 'int' and therefore does not need special
                 # handling introduced by @dr.syntax

           # Disable the transformation by @dr.syntax to avoid overheads
           while dr.hint(i < 10, method='scalar'):
               i += 1

    Complex Python codebases often involve successive application of multiple
    decorators to a function (e.g., combinations of ``@pytest.parameterize`` in
    a test suite). If one of these decorators is :py:func:`@drjit.syntax
    <drjit.syntax>`, then be sure to place it *closest* to the ``def``
    statement defining the function. Usually, decorators wrap one function into
    another one, but :py:func:`@drjit.syntax <drjit.syntax>` is special in that
    it rewrites the underlying code. If, *hypothetically*,
    :py:func:`@drjit.syntax <drjit.syntax>` was placed *above*
    ``@pytest.parameterize``, then it would rewrite the PyTest parameterization
    wrapper instead of the actual function definition, which is almost
    certainly not wanted.

    One last point: :py:func:`@dr.syntax <drjit.syntax>` may seem
    reminiscent of function--level transformations in other frameworks like
    ``@jax.jit`` (JAX) or ``@tf.function`` (TensorFlow). There is a key
    difference: these tools create a JIT compilation wrapper that intercepts
    calls and then invokes the nested function with placeholder arguments to
    compile and cache a kernel for each encountered combination of argument
    types. :py:func:`@dr.syntax <drjit.syntax>` is not like that: it
    merely rewrites the syntax of certain loop and conditional expressions and
    has no further effect following the function definition.
    """

    if f is None:

        def wrapper(f2):
            return syntax(f2, print_ast, print_code)

        return wrapper

    source = inspect.getsource(f)
    from textwrap import dedent
    source = dedent(source)

    old_ast = ast.parse(source)
    new_ast = old_ast
    if print_ast:
        print(f"Input AST\n---------\n{ast.dump(old_ast, indent=4)}\n")
    if print_code:
        print(f"Input code\n----------\n{ast.unparse(old_ast)}\n")

    new_ast = SyntaxVisitor().visit(old_ast)
    new_ast = ast.fix_missing_locations(new_ast)

    if print_ast:
        print(f"Output AST\n----------\n{ast.dump(new_ast, indent=4)}\n")
    if print_code:
        print(f"Output code\n-----------\n{ast.unparse(new_ast)}\n")

    old_code = f.__code__
    ast.increment_lineno(new_ast, old_code.co_firstlineno - 1)
    try:
        new_code = compile(new_ast, old_code.co_filename, "exec")
    except BaseException as e:
        raise RuntimeError(
            "The following transformed AST generated by "
            "@drjit.syntax could not be compiled:\n\n%s" % ast.unparse(new_ast)
        ) from e
    new_code = next(
        (x for x in new_code.co_consts if isinstance(x, types.CodeType)), None
    )
    return types.FunctionType(new_code, f.__globals__)


def hint(
    arg: object,
    /,
    *,
    method: Optional[str] = None,
    max_iterations: Optional[int] = None,
    label: Optional[str] = None,
    exclude: Optional[List[object]] = None,
) -> object:
    """
    Within ordinary Python code, this function is unremarkable: it returns the
    positional-only argument `arg` while ignoring any specified keyword
    arguments.

    The main purpose of :py:func:`drjit.hint()` is to provide *hints* that
    influence the transformation performed by the :py:func:`@drjit.syntax
    <drjit.syntax>` decorator. The following kinds of hints are supported:

    1. ``method`` overrides the compilation mode of a ``while``
       loop or ``if`` statement. The following choices are available:

       - ``method='scalar'`` disables code transformations, which is permitted
         when the predicate of a loop or ``if`` statement is a scalar Python
         ``bool``.

         .. code-block:: python

            i: int = 0
            while dr.hint(i < 10, method='scalar'):
               # ...

         Routing such code through :py:func:`drjit.while_loop` or
         :py:func:`drjit.if_stmt` still works but may add small overheads,
         which motivates the existence of this flag. Note that this annotation
         does *not* cause ``method=scalar`` to be passed
         :py:func:`drjit.while_loop`, and :py:func:`drjit.if_stmt` (which
         happens to be a valid input of both). Instead, it disables the code
         transformation altogether so that the above example translates into
         ordinary Python code:

         .. code-block:: python

            i: int = 0
            while i < 10:
               # ...

       - ``method='evaluated'`` forces execution in *evaluated* mode and causes
         the code transformation to forward this argument to the relevant
         :py:func:`drjit.while_loop` or :py:func:`drjit.if_stmt` call.

         Refer to the discussion of :py:func:`drjit.while_loop`,
         :py:attr:`drjit.JitFlag.SymbolicLoops`, :py:func:`drjit.if_stmt`, and
         :py:attr:`drjit.JitFlag.SymbolicConditionals` for details.

       - ``method='symbolic'`` forces execution in *symbolic* mode and causes
         the code transformation to forward this argument to the relevant
         :py:func:`drjit.while_loop` or :py:func:`drjit.if_stmt` call.

         Refer to the discussion of :py:func:`drjit.while_loop`,
         :py:attr:`drjit.JitFlag.SymbolicLoops`, :py:func:`drjit.if_stmt`, and
         :py:attr:`drjit.JitFlag.SymbolicConditionals` for details.

    2. ``max_iterations`` specifies a maximum number of loop iterations for
       reverse-mode automatic differentiation.

       Naive reverse-mode differentiation of loops (unless replaced by a
       smarter problem-specific strategy via :py:class:`drjit.custom` and
       :py:class:`drjit.CustomOp`) requires allocation of large buffers that
       hold loop state for all iterations.

       Dr.Jit requires an upper bound on the maximum number of loop iterations
       so that it can allocate such buffers, which can be provided via this
       hint. Otherwise, reverse-mode differentiation of loops will fail with an
       error message.

    3. ``label`` provovides a descriptive label.

       Dr.Jit will include this label as a comment in the generated
       intermediate representation, which can be helpful when debugging the
       compilation of large programs.

    4. ``exclude`` indicates to the :py:func:`@drjit.syntax <drjit.syntax>`
       decorator that a local variable should not be considered to be part of
       the set of state variables passed to :py:func:`drjit.while_loop` or
       :py:func:`drjit.if_stmt`.

       While transforming a function, the :py:func:`@drjit.syntax
       <drjit.syntax>` decorator sequentially steps through a program to
       identify the set of read and written variables. It then forwards
       referenced variables to recursive :py:func:`drjit.while_loop` and
       :py:func:`drjit.if_stmt` calls. In rare cases, it may be useful to
       exclude a local variable from this process--- specify a list of such
       variables to the :py:func:`drjit.hint` annotation to do so.
    """
    return arg
