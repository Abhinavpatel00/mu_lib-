# Transforming normals: cofactor, not inverse-transpose

`mu_mat3_cofactor()` and `mu_mat4_normal()` in `mu_math.h`.

## The bug the standard recipe has

`transpose(inverse(M)) * n` is the recipe every graphics text gives. It is
derived from the constraint `n' · t' = 0` (normal orthogonal to a tangent), which
does **not** determine orientation: it admits two solutions, and it silently
picks the wrong one when `det(M) < 0`. For a reflection, the transformed normal
points *into* the surface. Every light then evaluates `max(0, dot(n, l)) = 0` and
the object is lit as if it were inside out.

The correct operator is the **cofactor matrix**, `C = det(A) · inverse(A)^T`,
which differs from `inverse(A)^T` exactly by the sign of the determinant. For a
matrix with columns `a0, a1, a2`:

```
C = [ a1 x a2 , a2 x a0 , a0 x a1 ]        (as columns)
```

because `inverse(A)^T` is those same three cross products divided by `det(A)`.

Source: graphitemaster, [normals_revisited](https://github.com/graphitemaster/normals_revisited),
which ships the cofactor code this implements.

## It is also faster

Measured with `mu_bench.h` (`perf_test/bench_normals.c`, 64 non-uniform, skewed
matrices, every third one reflecting, 21 samples, one pinned core):

| path | ns/op (median) | cv |
| --- | --- | --- |
| `mu_mat4_inverse` then transpose then multiply | 11.03 | 1.2% |
| **`mu_mat4_normal` (cofactor)** | **3.58** | **1.1%** |
| `mu_mat3_inverse_transpose` then multiply | 3.83 | 1.7% |
| `mu_mat3_cofactor` then multiply | 3.39 | 4.5% |

⇒ **3.08x** on 4x4, **1.13x** on 3x3. (`make -C perf_test bench-normals`, 21
samples, confidence cutoff 6%, one pinned core; the earlier 11-sample run read
3.82 ns at cv 13.6% and was marked UNSTABLE - the number got *better* and
stabler with more samples, which is why an unstable row is never quoted.)

The 4x4 path is where the win is: a full 4x4 inverse costs far more than three
cross products and a 3x3 multiply. Against the 3x3 inverse the speed win is
small, but the *correctness* win is not - the 3x3 inverse-transpose has exactly
the same reflection bug.

## Use

```c
mu_vec3 n2 = mu_vec3_normalize(mu_mat4_normal(&model, n));   /* C scales: normalize */
```

* If the matrix is a rotation, `C == A`; transform with the matrix and skip this.
* If the scale is uniform, `C == s^2 · A`.
* The translation column cannot affect a direction and is never read.
* The result is **not** normalized.

## Gate

`perf_test/test_math.c` asserts, over 300 random invertible matrices:

* `mu_mat3_cofactor == det · mu_mat3_inverse_transpose` (so the algebra is right);
* the cofactor normal of a transformed triangle is the same direction as the
  normal recomputed from the transformed vertices (the property that matters);
* the reflection case explicitly: cofactor gives `-z`, and `inverse(M)^T` gives
  `+z` - the second assertion exists so that "simplifying" `mu_mat4_normal` back
  to an inverse-transpose fails the build with an explanation.
