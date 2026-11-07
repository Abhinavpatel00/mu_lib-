┌─────────────────────────────────────────────────────────────┐
│                    PICK BY ACCESS PATTERN                   │
├─────────────────────┬───────────────────────────────────────┤
│ Need                │ Use                                   │
├─────────────────────┼───────────────────────────────────────┤
│ Dense hot iteration │ flat array / vector / frame arena     │
│ Stable handles      │ mu_bulk_storage                       │
│ Dense active IDs    │ mu_sparse_set                         │
│ Sparse 64-bit IDs   │ mu_sparse_array                       │
│ Unique work queue   │ mu_worklist                           │
│ Fixed-size churn    │ mu_slab                               │
│ Delayed destruction │ mu_deferred_free                      │
│ Ordered tree        │ mu_rb_tree                            │
│ Overlapping ranges  │ mu_rb_tree interval tree              │
│ Reclaimable slabs   │ mu_slab_manager                       │
│ Dense/ranged IDs    │ mu_id_alloc                           │
│ Sparse ID sets      │ mu_sparse_bitset                      │
│ Dependency graph    │ mu_dag                                │
│ Range -> pointer    │ mu_range_remap                        │
│ key -> one value    │ mu_hash32_static / hash_t             │
│ static keys, O(1)   │ mu_phf                                │
│ key -> many values  │ mu_multi_index                        │
│ packed booleans     │ mu_bitset                             │
│ ranged IDs          │ mu_id_pool                            │
│ variable children   │ mu_chunked_u32_array                  │
│ packed strings      │ mu_string_arena                       │
│ intrusive lists     │ mu_pool_link / mu_index_list          │
└─────────────────────┴───────────────────────────────────────┘
