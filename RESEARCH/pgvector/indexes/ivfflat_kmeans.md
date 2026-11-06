# k-Means Clustering Engine in pgvector (`ivfkmeans.c`)

## Table of Contents
1. [Overview & Role in IVFFlat](#1-overview--role-in-ivfflat)
2. [Mathematical Foundations & Triangle Inequality](#2-mathematical-foundations--triangle-inequality)
   - [Euclidean Distance vs. Squared Euclidean Distance](#euclidean-distance-vs-squared-euclidean-distance)
   - [Spherical k-means for Cosine & Inner Product](#spherical-k-means-for-cosine--inner-product)
   - [Elkan's Triangle Inequality Lemmas](#elkans-triangle-inequality-lemmas)
3. [Centroid Initialization: k-means++ (`InitCenters`)](#3-centroid-initialization-k-means-initcenters)
   - [Algorithm Description & Probabilistic Formulation](#algorithm-description--probabilistic-formulation)
   - [C Implementation Walkthrough](#c-implementation-walkthrough)
   - [Dual Purpose: Seeding Elkan's Initial Lower Bounds](#dual-purpose-seeding-elkans-initial-lower-bounds)
4. [Elkan's Accelerated k-means Algorithm (`ElkanKmeans`)](#4-elkans-accelerated-k-means-algorithm-elkankmeans)
   - [Memory Allocation & Bound Arrays](#memory-allocation--bound-arrays)
   - [The 7-Step Iteration Loop](#the-7-step-iteration-loop)
   - [Complete Source Code of the Iteration Loop](#complete-source-code-of-the-iteration-loop)
   - [Why Elkan Prunes >80-90% of Distance Calculations](#why-elkan-prunes-80-90-of-distance-calculations)
5. [Convergence & Stopping Criteria](#5-convergence--stopping-criteria)
   - [Assignment Stability (`changes == 0`)](#assignment-stability-changes--0)
   - [Iteration Ceiling (500 iterations)](#iteration-ceiling-500-iterations)
   - [Interrupt Responsiveness (`CHECK_FOR_INTERRUPTS`)](#interrupt-responsiveness-check_for_interrupts)
6. [Numerical Stability & Edge Cases](#6-numerical-stability--edge-cases)
   - [Empty Cluster Re-seeding](#empty-cluster-re-seeding)
   - [Infinite & NaN Coordinate Clamping](#infinite--nan-coordinate-clamping)
   - [Zero-Vector Protection for Cosine Metrics (`CheckNorms`)](#zero-vector-protection-for-cosine-metrics-checknorms)
   - [Zero-Sample Fallback (`RandomCenters`)](#zero-sample-fallback-randomcenters)
7. [Algorithm Complexity & Benchmark Metrics](#7-algorithm-complexity--benchmark-metrics)
   - [Standard Lloyd's vs. Elkan Complexity](#standard-lloyds-vs-elkan-complexity)
   - [Space Complexity & Huge Allocation Handling (`MCXT_ALLOC_HUGE`)](#space-complexity--huge-allocation-handling-mcxt_alloc_huge)
   - [Clustering Quality Metrics: Inertia & Davies-Bouldin Index](#clustering-quality-metrics-inertia--davies-bouldin-index)

---

## 1. Overview & Role in IVFFlat

In an IVFFlat index, search performance and recall hinge entirely on the quality of its Voronoi cell partitions. The centroid computation engine in [ivfkmeans.c](file:///mnt/Data/Coding/arkivector/pgvector/src/ivfkmeans.c) calculates the coordinates of all $K$ centroids ($\mathbf{c}_1, \dots, \mathbf{c}_K$) from the sampled dataset vectors ($\mathbf{x}_1, \dots, \mathbf{x}_N$).

Rather than relying on naive Lloyd's relaxation ($\mathcal{O}(I \cdot N \cdot K \cdot d)$), pgvector implements **Charles Elkan's accelerated $k$-means algorithm** (ICML 2003) initialized with **$k$-means++** (Arthur & Vassilvitskii 2007).

```
                      Centroid Computation Pipeline
+-------------------------------------------------------------------------+
| Vector Sample S (N vectors, dimensionality d)                           |
+-------------------------------------------------------------------------+
                                    |
                                    v
+-------------------------------------------------------------------------+
| k-means++ Initialization (InitCenters)                                  |
| 1. Pick first centroid uniformly at random: c_0 ~ Uniform(S)            |
| 2. For k = 1 to K-1:                                                    |
|    Sample next centroid with probability P(x) ~ D(x)^2                  |
| 3. Simultaneously populate initial Elkan lowerBound[j * K + k] matrix   |
+-------------------------------------------------------------------------+
                                    |
                                    v
+-------------------------------------------------------------------------+
| Elkan's Accelerated Iteration Loop (Max 500 iterations)                 |
| Step 1: Compute center-center distances & s(c) = 0.5 * min d(c, c')    |
| Step 2: Skip point x if u(x) <= s(c(x))                                 |
| Step 3: For remaining points, prune candidate centers using:            |
|         - Lower bound check: u(x) <= l(x, c)                            |
|         - Triangle inequality check: u(x) <= 0.5 * d(c(x), c)           |
|         - Exact bound tightening & center reassignment                  |
| Step 4: Compute new centers m(c) via vector summation                   |
| Step 5: Update lower bounds: l(x, c) = max(0, l(x, c) - d(c, m(c)))     |
| Step 6: Update upper bounds: u(x) = u(x) + d(c(x), m(c(x)))             |
| Step 7: Check convergence (changes == 0)                                |
+-------------------------------------------------------------------------+
                                    |
                                    v
+-------------------------------------------------------------------------+
| Validation & Sanity Verification (CheckCenters)                         |
| 1. Verify exact count: centers->length == centers->maxlen               |
| 2. Detect NaNs or Infs across all dimensions (CheckElements)            |
| 3. Ensure no zero-norm vectors exist for cosine opclasses (CheckNorms)  |
+-------------------------------------------------------------------------+
```

---

## 2. Mathematical Foundations & Triangle Inequality

Elkan's algorithm requires that the distance function $\mathcal{D}(\mathbf{x}, \mathbf{y})$ satisfies the standard **triangle inequality**:
$$\mathcal{D}(\mathbf{x}, \mathbf{z}) \le \mathcal{D}(\mathbf{x}, \mathbf{y}) + \mathcal{D}(\mathbf{y}, \mathbf{z}) \quad \forall \mathbf{x}, \mathbf{y}, \mathbf{z} \in \mathbb{R}^d$$

### Euclidean Distance vs. Squared Euclidean Distance
During index scan execution, pgvector uses **squared Euclidean distance** ($L_2^2$):
$$\mathcal{D}_{\text{scan}}(\mathbf{x}, \mathbf{y}) = \sum_{i=1}^d (x_i - y_i)^2$$
This avoids computing expensive square root instructions ($\sqrt{\cdot}$) on every distance calculation.

**Crucial Insight:** Squared Euclidean distance **violates** the triangle inequality!
*Counterexample:* In 1D, let $x = 0$, $y = 1$, and $z = 2$.
- $\mathcal{D}_{L_2^2}(x, y) = (0 - 1)^2 = 1$
- $\mathcal{D}_{L_2^2}(y, z) = (1 - 2)^2 = 1$
- $\mathcal{D}_{L_2^2}(x, z) = (0 - 2)^2 = 4$
- Here, $\mathcal{D}(x, z) = 4 > \mathcal{D}(x, y) + \mathcal{D}(y, z) = 2$. The triangle inequality fails.

If Elkan's pruning were executed over squared distances, geometric bounds would be mathematically invalid, leading to incorrect cluster assignments.

To resolve this, pgvector registers two distinct support procedures in PostgreSQL catalogs:
- **Procedure 1 (`IVFFLAT_DISTANCE_PROC`):** `vector_l2_squared_distance` (used during scan and insert).
- **Procedure 3 (`IVFFLAT_KMEANS_DISTANCE_PROC`):** `l2_distance` (true Euclidean distance $\sqrt{\sum (x_i - y_i)^2}$, used exclusively during $k$-means).

### Spherical k-means for Cosine & Inner Product
For cosine similarity and inner product operator classes (`vector_cosine_ops`, `vector_ip_ops`), the vectors live on the unit hypersphere $\mathbb{S}^{d-1}$.
1. **Metric:** pgvector registers `vector_spherical_distance` as Procedure 3. This computes **angular distance**:
   $$\theta(\mathbf{x}, \mathbf{y}) = \arccos\left( \frac{\mathbf{x} \cdot \mathbf{y}}{\|\mathbf{x}\| \|\mathbf{y}\|} \right)$$
   Angular distance defines a true geodesic metric on the sphere and strictly satisfies the triangle inequality.
2. **Spherical Projection:** In each iteration, when centroid coordinate averages are updated in `ComputeNewCenters`, centroids are projected back onto the unit sphere via `NormCenters` (`IVFFLAT_KMEANS_NORM_PROC`):
   $$\mathbf{c}_k \gets \frac{\mathbf{c}_k}{\|\mathbf{c}_k\|_2}$$

### Elkan's Triangle Inequality Lemmas
Elkan derives two fundamental lemmas that allow skipping distance computations:

#### Lemma 1 (Center-Center Half Distance)
Let $\mathbf{x}$ be a sample point, $\mathbf{c}$ be its currently assigned centroid, and $\mathbf{c}'$ be another centroid.
$$\text{If } \frac{1}{2} \mathcal{D}(\mathbf{c}, \mathbf{c}') \ge \mathcal{D}(\mathbf{x}, \mathbf{c}), \text{ then } \mathcal{D}(\mathbf{x}, \mathbf{c}') \ge \mathcal{D}(\mathbf{x}, \mathbf{c})$$
*Proof:* By the triangle inequality:
$$\mathcal{D}(\mathbf{c}, \mathbf{c}') \le \mathcal{D}(\mathbf{x}, \mathbf{c}) + \mathcal{D}(\mathbf{x}, \mathbf{c}') \implies \mathcal{D}(\mathbf{x}, \mathbf{c}') \ge \mathcal{D}(\mathbf{c}, \mathbf{c}') - \mathcal{D}(\mathbf{x}, \mathbf{c})$$
Substituting $\mathcal{D}(\mathbf{c}, \mathbf{c}') \ge 2 \mathcal{D}(\mathbf{x}, \mathbf{c})$:
$$\mathcal{D}(\mathbf{x}, \mathbf{c}') \ge 2 \mathcal{D}(\mathbf{x}, \mathbf{c}) - \mathcal{D}(\mathbf{x}, \mathbf{c}) = \mathcal{D}(\mathbf{x}, \mathbf{c})$$
Therefore, $\mathbf{c}'$ cannot be closer to $\mathbf{x}$ than $\mathbf{c}$, and $\mathcal{D}(\mathbf{x}, \mathbf{c}')$ never needs to be calculated.

#### Lemma 2 (Lower and Upper Bound Pruning)
Let $u(\mathbf{x})$ be an upper bound on $\mathcal{D}(\mathbf{x}, \mathbf{c}(\mathbf{x}))$, and let $l(\mathbf{x}, \mathbf{c}')$ be a lower bound on $\mathcal{D}(\mathbf{x}, \mathbf{c}')$.
$$\text{If } u(\mathbf{x}) \le l(\mathbf{x}, \mathbf{c}'), \text{ then } \mathcal{D}(\mathbf{x}, \mathbf{c}(\mathbf{x})) \le \mathcal{D}(\mathbf{x}, \mathbf{c}')$$
Centroid $\mathbf{c}'$ cannot be closer to $\mathbf{x}$ than its current assignment, and can be skipped without evaluating exact vector distance.

---

## 3. Centroid Initialization: k-means++ (`InitCenters`)

Poor initial centroids lead standard $k$-means to converge to suboptimal local minima. pgvector uses **$k$-means++** to choose well-separated starting centroids.

### Algorithm Description & Probabilistic Formulation
1. Choose the first centroid $\mathbf{c}_0$ uniformly at random from the sampled vectors:
   $$\mathbf{c}_0 \sim \text{Uniform}(\mathbf{x}_1, \dots, \mathbf{x}_N)$$
2. For each sample vector $\mathbf{x}_j$, track the shortest squared distance to any already-chosen centroid:
   $$D(\mathbf{x}_j)^2 = \min_{m=0}^{i-1} \mathcal{D}(\mathbf{x}_j, \mathbf{c}_m)^2$$
3. Select the next centroid $\mathbf{c}_i$ from the remaining vectors with probability proportional to $D(\mathbf{x}_j)^2$:
   $$P(\mathbf{x}_j \text{ is chosen}) = \frac{D(\mathbf{x}_j)^2}{\sum_{m=1}^N D(\mathbf{x}_m)^2}$$
4. Repeat until all $K$ centroids are chosen.

### C Implementation Walkthrough
In [ivfkmeans.c](file:///mnt/Data/Coding/arkivector/pgvector/src/ivfkmeans.c#L23-L91):

```c
static void
InitCenters(Relation index, VectorArray samples, VectorArray centers, float *lowerBound)
{
    FmgrInfo *procinfo;
    Oid       collation;
    float    *weight = palloc_array_checked(float, samples->length);
    int       numCenters = centers->maxlen;
    int       numSamples = samples->length;

    procinfo = index_getprocinfo(index, 1, IVFFLAT_KMEANS_DISTANCE_PROC);
    collation = index->rd_indcollation[0];

    /* Choose an initial center uniformly at random */
    VectorArraySet(centers, 0, VectorArrayGet(samples, RandomInt() % samples->length));
    centers->length++;

    for (int i = 0; i < numSamples; i++)
        weight[i] = FLT_MAX;

    for (int i = 0; i < numCenters; i++)
    {
        int    j;
        double sum;
        double choice;

        CHECK_FOR_INTERRUPTS();
        sum = 0.0;

        for (j = 0; j < numSamples; j++)
        {
            Datum  vec = PointerGetDatum(VectorArrayGet(samples, j));
            double distance;

            /* Only compute distance to the newest centroid */
            distance = DatumGetFloat8(FunctionCall2Coll(procinfo, collation, vec, 
                                                        PointerGetDatum(VectorArrayGet(centers, i))));

            /* Save distance as initial lower bound for Elkan's algorithm! */
            lowerBound[(Size) j * numCenters + i] = distance;

            /* Square distance for probability distribution weighting */
            distance *= distance;

            if (distance < weight[j])
                weight[j] = distance;

            sum += weight[j];
        }

        if (i + 1 == numCenters)
            break;

        /* Pick next centroid using weighted cumulative probability distribution */
        choice = sum * RandomDouble();
        for (j = 0; j < numSamples - 1; j++)
        {
            choice -= weight[j];
            if (choice <= 0)
                break;
        }

        VectorArraySet(centers, i + 1, VectorArrayGet(samples, j));
        centers->length++;
    }

    pfree(weight);
}
```

### Dual Purpose: Seeding Elkan's Initial Lower Bounds
Notice line 62:
```c
lowerBound[(Size) j * numCenters + i] = distance;
```
During the $k$-means++ selection loop, pgvector already calculates the exact Euclidean distance between each vector $\mathbf{x}_j$ and centroid $\mathbf{c}_i$. Rather than discarding these expensive calculations, pgvector directly writes them into Elkan's `lowerBound` matrix. When Elkan begins iteration 0, all lower bounds $l(\mathbf{x}_j, \mathbf{c}_i)$ are already pre-initialized for free.

---

## 4. Elkan's Accelerated k-means Algorithm (`ElkanKmeans`)

### Memory Allocation & Bound Arrays
Elkan's algorithm requires several auxiliary arrays ([ivfkmeans.c](file:///mnt/Data/Coding/arkivector/pgvector/src/ivfkmeans.c#L247-L319)):

| Variable | Type | Dimensions | Purpose |
|---|---|---|---|
| `lowerBound` | `float*` | $N \times K$ | Lower bound $l(\mathbf{x}_j, \mathbf{c}_k)$ on distance from point $j$ to center $k$. |
| `upperBound` | `float*` | $N$ | Upper bound $u(\mathbf{x}_j)$ on distance from point $j$ to its assigned center. |
| `closestCenters` | `int*` | $N$ | Index $c(\mathbf{x}_j)$ of the closest centroid for sample $j$. |
| `centerCounts` | `int*` | $K$ | Number of sample vectors assigned to centroid $k$. |
| `agg` | `float*` | $K \times d$ | Coordinate accumulator summing assigned vectors per centroid. |
| `halfcdist` | `float*` | $K \times K$ | Matrix of half center-center distances: $\frac{1}{2} \mathcal{D}(\mathbf{c}_j, \mathbf{c}_k)$. |
| `s` | `float*` | $K$ | Vector of minimum half center distances: $s(\mathbf{c}_j) = \min_{k \ne j} \text{halfcdist}[j, k]$. |
| `newcdist` | `float*` | $K$ | Distance shifted by each center between iterations: $\mathcal{D}(\mathbf{c}_k^{(t)}, \mathbf{c}_k^{(t+1)})$. |

### The 7-Step Iteration Loop
The iteration executes up to 500 times:

```mermaid
flowchart TD
    subgraph Step1 ["Step 1: Center-Center Distances"]
        S1["Compute halfcdist[j, k] = 0.5 * D(c_j, c_k)<br>Compute s[j] = min_{k != j} halfcdist[j, k]"]
    end
    
    subgraph Step23 ["Steps 2 & 3: Sample Assignment & Pruning"]
        S2["For each sample x_j:<br>Check upperBound[j] <= s[closestCenter[j]]"]
        S2 -- True --> SkipAll["Skip point x_j entirely (Lemma 1)"]
        S2 -- False --> CheckCenters["For each centroid k != c(x_j):<br>Check upperBound[j] <= lowerBound[j, k]<br>Check upperBound[j] <= halfcdist[c(x), k]"]
        CheckCenters -- Pruned --> SkipK["Skip centroid k"]
        CheckCenters -- Not Pruned --> Tighten["Step 3a: Tighten upperBound[j] via exact D(x_j, c(x_j))<br>Step 3b: If still not pruned, compute exact D(x_j, c_k)"]
        Tighten --> Reassign{"D(x_j, c_k) < D(x_j, c(x))?"}
        Reassign -- Yes --> UpdateAssign["closestCenter[j] = k<br>upperBound[j] = distance<br>changes++"]
        Reassign -- No --> KeepAssign["lowerBound[j, k] = distance"]
    end

    subgraph Step47 ["Steps 4-7: Updating Centroids & Bounds"]
        UpdateAssign --> S4["Step 4: ComputeNewCenters()<br>agg sum / count per cluster"]
        KeepAssign --> S4
        SkipAll --> S4
        SkipK --> S4
        S4 --> S5["Step 5: newcdist[j] = D(old_c_j, new_c_j)<br>lowerBound[j, k] -= newcdist[k]"]
        S5 --> S6["Step 6: upperBound[j] += newcdist[closestCenter[j]]"]
        S6 --> S7["Step 7: centers = newCenters<br>If changes == 0: BREAK"]
    end
```

### Complete Source Code of the Iteration Loop
From [ivfkmeans.c](file:///mnt/Data/Coding/arkivector/pgvector/src/ivfkmeans.c#L347-L485):

```c
/* Give 500 iterations to converge */
for (int iteration = 0; iteration < 500; iteration++)
{
    int  changes = 0;
    bool rjreset;

    CHECK_FOR_INTERRUPTS();

    /* Step 1: For all centers, compute pairwise distance */
    for (int j = 0; j < numCenters; j++)
    {
        Datum vec = PointerGetDatum(VectorArrayGet(centers, j));

        for (int k = j + 1; k < numCenters; k++)
        {
            float distance = 0.5 * DatumGetFloat8(FunctionCall2Coll(procinfo, collation, vec, 
                                                                    PointerGetDatum(VectorArrayGet(centers, k))));

            halfcdist[(Size) j * numCenters + k] = distance;
            halfcdist[(Size) k * numCenters + j] = distance;
        }
    }

    /* For all centers c, compute s(c) = min_{k != c} 0.5 * d(c, k) */
    for (int j = 0; j < numCenters; j++)
    {
        float minDistance = FLT_MAX;

        for (int k = 0; k < numCenters; k++)
        {
            float distance;

            if (j == k)
                continue;

            distance = halfcdist[(Size) j * numCenters + k];
            if (distance < minDistance)
                minDistance = distance;
        }

        s[j] = minDistance;
    }

    rjreset = iteration != 0;

    for (int j = 0; j < numSamples; j++)
    {
        bool rj;

        /* Step 2: Identify all points x such that u(x) <= s(c(x)) */
        if (upperBound[j] <= s[closestCenters[j]])
            continue;

        rj = rjreset;

        for (int k = 0; k < numCenters; k++)
        {
            Datum vec;
            float dxcx;

            /* Step 3: For all remaining points x and centers c */
            if (k == closestCenters[j])
                continue;

            if (upperBound[j] <= lowerBound[(Size) j * numCenters + k])
                continue;

            if (upperBound[j] <= halfcdist[(Size) closestCenters[j] * numCenters + k])
                continue;

            vec = PointerGetDatum(VectorArrayGet(samples, j));

            /* Step 3a: Tighten upper bound by computing true d(x, c(x)) */
            if (rj)
            {
                dxcx = DatumGetFloat8(FunctionCall2Coll(procinfo, collation, vec, 
                                                        PointerGetDatum(VectorArrayGet(centers, closestCenters[j]))));

                lowerBound[(Size) j * numCenters + closestCenters[j]] = dxcx;
                upperBound[j] = dxcx;
                rj = false;
            }
            else
                dxcx = upperBound[j];

            /* Step 3b: If still not pruned, compute exact d(x, c) */
            if (dxcx > lowerBound[(Size) j * numCenters + k] || 
                dxcx > halfcdist[(Size) closestCenters[j] * numCenters + k])
            {
                float dxc = DatumGetFloat8(FunctionCall2Coll(procinfo, collation, vec, 
                                                             PointerGetDatum(VectorArrayGet(centers, k))));

                lowerBound[(Size) j * numCenters + k] = dxc;

                if (dxc < dxcx)
                {
                    closestCenters[j] = k;
                    upperBound[j] = dxc;
                    changes++;
                }
            }
        }
    }

    /* Step 4: Compute new centers m(c) */
    ComputeNewCenters(samples, agg, newCenters, centerCounts, closestCenters, normprocinfo, collation, typeInfo);

    /* Step 5: Compute distance each center shifted: d(c, m(c)) */
    for (int j = 0; j < numCenters; j++)
        newcdist[j] = DatumGetFloat8(FunctionCall2Coll(procinfo, collation, 
                                                       PointerGetDatum(VectorArrayGet(centers, j)), 
                                                       PointerGetDatum(VectorArrayGet(newCenters, j))));

    for (int j = 0; j < numSamples; j++)
    {
        for (int k = 0; k < numCenters; k++)
        {
            float distance = lowerBound[(Size) j * numCenters + k] - newcdist[k];
            if (distance < 0)
                distance = 0;
            lowerBound[(Size) j * numCenters + k] = distance;
        }
    }

    /* Step 6: Update upper bounds */
    for (int j = 0; j < numSamples; j++)
        upperBound[j] += newcdist[closestCenters[j]];

    /* Step 7: Update centers */
    for (int j = 0; j < numCenters; j++)
        VectorArraySet(centers, j, VectorArrayGet(newCenters, j));

    if (changes == 0 && iteration != 0)
        break;
}
```

### Why Elkan Prunes >80-90% of Distance Calculations
In standard Lloyd's algorithm, every iteration must evaluate $N \times K$ full vector distance calculations.
With Elkan's algorithm:
1. In Step 2, once clusters begin stabilizing, the vast majority of vectors satisfy $u(\mathbf{x}) \le s(c(\mathbf{x}))$. Entire blocks of samples bypass the inner centroid loop entirely.
2. In Step 3, the triangle inequality check against `halfcdist` and `lowerBound` prunes distant clusters without reading the vector coordinates.
3. In practice, after iterations 3–5, **over 95% of pairwise distance evaluations are skipped**, accelerating index build times by 5x to 20x.

---

## 5. Convergence & Stopping Criteria

1. **Zero Point Shifts (`changes == 0`):** If no sample point changed its assigned cluster between iterations, $k$-means has reached a local optimum. The loop terminates immediately via `break`.
2. **Iteration Ceiling (500 Iterations):** If subtle boundary oscillations prevent `changes` from reaching 0, the loop halts after 500 iterations, guaranteeing termination.
3. **Interrupt Responsiveness:** Long-running $k$-means operations on large sample sets can take several seconds. Every iteration explicitly executes `CHECK_FOR_INTERRUPTS()`, allowing administrators to cancel the operation via `pg_cancel_backend()`.

---

## 6. Numerical Stability & Edge Cases

High-dimensional floating-point algorithms are prone to degenerate edge cases:

### Empty Cluster Re-seeding
If a cluster ends up with zero assigned samples (`centerCounts[i] == 0`), dividing the accumulator by count would cause a division by zero. pgvector detects empty clusters in `ComputeNewCenters` ([ivfkmeans.c](file:///mnt/Data/Coding/arkivector/pgvector/src/ivfkmeans.c#L222-L227)) and re-seeds the centroid with random coordinates:
```c
if (centerCounts[i] > 0)
{
    for (int j = 0; j < dimensions; j++)
    {
        if (isinf(x[j]))
            x[j] = x[j] > 0 ? FLT_MAX : -FLT_MAX;
    }

    for (int j = 0; j < dimensions; j++)
        x[j] /= centerCounts[i];
}
else
{
    /* Re-randomize empty centroid */
    for (int j = 0; j < dimensions; j++)
        x[j] = RandomDouble();
}
```

### Infinite & NaN Coordinate Clamping
If high-dimensional additions overflow single-precision floating point limits, `isinf(x[j])` clamps the coordinate to `FLT_MAX` or `-FLT_MAX`. Furthermore, after clustering completes, `CheckElements` validates every centroid coordinate:
```c
for (int j = 0; j < centers->dim; j++)
{
    if (isnan(scratch[j]))
        elog(ERROR, "NaN detected. Please report a bug.");

    if (isinf(scratch[j]))
        elog(ERROR, "Infinite value detected. Please report a bug.");
}
```

### Zero-Vector Protection for Cosine Metrics (`CheckNorms`)
For cosine distance operator classes, a centroid with all-zero components ($\mathbf{0}$) causes division by zero during normalization. `CheckNorms` verifies that every centroid has a positive $L_2$ norm:
```c
double norm = DatumGetFloat8(FunctionCall1Coll(normprocinfo, collation, 
                                               PointerGetDatum(VectorArrayGet(centers, i))));
if (norm == 0)
    elog(ERROR, "Zero norm detected. Please report a bug.");
```

### Zero-Sample Fallback (`RandomCenters`)
When an index is built on an empty or newly truncated table (`samples->length == 0`), `IvfflatKmeans` bypasses Elkan entirely and invokes `RandomCenters`, filling the requested $K$ centroids with random uniformly distributed values in $[0, 1)$.

---

## 7. Algorithm Complexity & Benchmark Metrics

### Standard Lloyd's vs. Elkan Complexity

| Metric | Standard Lloyd's | Elkan's Algorithm (pgvector) |
|---|---|---|
| **Time Complexity** | $\mathcal{O}(I \cdot N \cdot K \cdot d)$ | $\mathcal{O}(N \cdot K + I \cdot (K^2 d + N_{\text{active}} \cdot K_{\text{cand}} \cdot d))$ |
| **Distance Evaluations** | $I \times N \times K$ (constant every iteration) | $\approx 10\% - 15\%$ of Lloyd's evaluations |
| **Space Overhead** | $\mathcal{O}(K \cdot d)$ | $\mathcal{O}(N \cdot K \text{ floats} + K^2 \text{ floats} + K \cdot d)$ |

### Space Complexity & Huge Allocation Handling (`MCXT_ALLOC_HUGE`)
The `lowerBound` matrix requires $N \times K \times \text{sizeof(float)}$ bytes.
For large indexes with $K = 10,000$ and $N = 50,000$ samples:
$$\text{Memory}_{\text{lowerBound}} = 50,000 \times 10,000 \times 4 \text{ bytes} \approx 2.0 \text{ GB}$$
Standard `palloc` in PostgreSQL is restricted to 1GB allocations (`MaxAllocSize`). To support high `lists` configurations without crashing, pgvector explicitly uses `palloc_extended` with the `MCXT_ALLOC_HUGE` flag:
```c
lowerBound = palloc_extended(lowerBoundSize, MCXT_ALLOC_HUGE);
halfcdist  = palloc_extended(halfcdistSize, MCXT_ALLOC_HUGE);
```
Additionally, integer multiplication overflow is explicitly guarded:
```c
if (numCenters > INT_MAX / numCenters)
    elog(ERROR, "Indexing overflow detected. Please report a bug.");
```

### Clustering Quality Metrics: Inertia & Davies-Bouldin Index
When compiled with `-DIVFFLAT_KMEANS_DEBUG`, pgvector instruments the build state with two clustering evaluation metrics:

1. **Inertia (Within-Cluster Sum of Squares):**
   $$\text{Inertia} = \sum_{j=1}^N \min_{k=1}^K \mathcal{D}(\mathbf{x}_j, \mathbf{c}_k)^2$$
   Measures total distortion. Lower values denote tighter, more coherent clusters.
2. **Davies-Bouldin Index:**
   $$R_{ij} = \frac{\bar{d}_i + \bar{d}_j}{\mathcal{D}(\mathbf{c}_i, \mathbf{c}_j)}, \quad DB = \frac{1}{K} \sum_{i=1}^K \max_{j \ne i} R_{ij}$$
   where $\bar{d}_i$ is the average distance from points in cluster $i$ to centroid $\mathbf{c}_i$. Lower values indicate superior cluster separation and compactness.
