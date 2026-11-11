# Model configs

One JSON file per model. Each file is copied into every artifact and run report.

`recs-train train` looks up `<model>.<dataset>.tuned.json` first, then `<model>.json`, then the
model's code defaults. A `.tuned.json` file holds hyperparameters chosen on the validation split
for one dataset. `<model>.json` holds the starting values (spec defaults where the spec gives one,
or tuned ml-latest-small values where noted below). The test split is evaluated once per final
config.

## knn

`knn.json` is the ml-latest-small config and matches the code defaults. `knn.ml-32m.tuned.json` is
the ml-32m config. Both use the spec's top-N score (`normalize: false`, `positive_only: true`).
M (`neighbors`) and λ (`shrinkage`) were chosen on the **validation** split only (split seed 42 and
model and bootstrap seed 42, `min_support` 20). The ml-32m search is in `docs/phase-1.md`.

ml-latest-small validation NDCG@10 (610 users), top-N form, full grid. Each cell is one run of:

```bash
build/release/train/recs-train eval --dataset ml-latest-small --model knn --on validation --split-seed 42 --seed 42 \
  --config <(echo '{"neighbors": M, "shrinkage": λ, "min_support": 20, "normalize": false, "positive_only": true}')
```

| M \ λ | 0 | 10 | 25 | 50 | 100 | 300 | 1,000 | 3,000 | 10,000 | 30,000 |
|---|---|---|---|---|---|---|---|---|---|---|
| 5 | 0.0111 | 0.0320 | 0.0457 | 0.0578 | 0.0631 | 0.0723 | 0.0770 | 0.0774 | 0.0770 | 0.0770 |
| **10** | 0.0126 | 0.0314 | 0.0429 | 0.0570 | 0.0657 | 0.0761 | **0.0783** | 0.0774 | 0.0774 | 0.0777 |
| 20 | 0.0146 | 0.0331 | 0.0437 | 0.0579 | 0.0592 | 0.0704 | 0.0725 | 0.0736 | 0.0745 | 0.0739 |
| 50 | 0.0189 | 0.0291 | 0.0465 | 0.0513 | 0.0608 | 0.0694 | 0.0732 | 0.0735 | 0.0743 | 0.0744 |
| 100 | 0.0201 | 0.0311 | 0.0417 | 0.0540 | 0.0608 | 0.0649 | 0.0675 | 0.0685 | 0.0685 | 0.0688 |
| 200 | 0.0130 | 0.0268 | 0.0419 | 0.0545 | 0.0605 | 0.0684 | 0.0686 | 0.0704 | 0.0705 | 0.0706 |
| 500 | 0.0058 | 0.0303 | 0.0482 | 0.0596 | 0.0660 | 0.0696 | 0.0718 | 0.0723 | 0.0726 | 0.0724 |

Chosen: M = 10, λ = 1,000 (validation NDCG@10 0.0783 [0.0647, 0.0934], Recall@50 0.1858,
Coverage@10 0.065). Coverage@10 falls from about 0.11 at λ = 0 to about 0.06 for λ ≥ 1,000.
The confidence intervals of the top cells overlap; the argmax is used as is.

The four scoring forms on the same validation split, M = 100, λ = 100:

| Form | NDCG@10 | Coverage@10 |
|---|---|---|
| normalize, all neighbours (rating prediction) | 0.0026 | 0.108 |
| normalize, positive only | 0.0065 | 0.108 |
| unnormalized, all neighbours | 0.0578 | 0.065 |
| unnormalized, positive only (top-N) | 0.0608 | 0.066 |
| baseline pop_count | 0.0589 | 0.012 |
| baseline pop (m = 10) | 0.0355 | 0.007 |

## mf

`mf.json` is the ml-latest-small config and pins every key (the code defaults match it).

- `k`, `lambda`, `lr`, `lr_decay`, `max_epochs`, `patience`, `init_sd`, `user_init_sd` were chosen
  on the validation split for the best RMSE subject to the fold-in parity gate (> 0.9); see the
  "Convergence and fold-in parity" comment in `core/src/models/mf_sgd.cpp`.
- `blocks` (DSGD grid side) and `precision` only change speed and, slightly, the update order and
  rounding. On ml-latest-small validation (split seed 42, seed 42), with the other keys as in
  `mf.json` and no floor:

| blocks | precision | RMSE | fold-in parity |
|---|---|---|---|
| 1 | double | 0.8961 | 0.928 |
| 8 | double | 0.8955 | 0.916 |
| 1 | float | 0.8961 | 0.928 |
| 8 | float | 0.8955 | 0.916 |

- `min_support` (ranking floor) is 0 on ml-latest-small. MF ranks poorly on this dataset:
  validation NDCG@10 rises with the floor only because the list turns into a popularity list,
  and with the floor the parity gate (measured on the floored lists) fails for mid-range values.
  Serial double, same split:

| min_support | NDCG@10 | mean popularity rank @10 | coverage@10 | parity |
|---|---|---|---|---|
| 0 | 0.0012 | 5387 | 0.022 | 0.928 |
| 5 | 0.0040 | 2648 | 0.028 | 0.916 |
| 10 | 0.0105 | 1224 | 0.034 | 0.897 |
| 20 | 0.0202 | 479 | 0.033 | 0.885 |
| 50 | 0.0375 | 138 | 0.022 | 0.888 |
| 100 | 0.0472 | 44 | 0.010 | 0.925 |
| 150 | 0.0514 | 17 | 0.004 | 0.949 |

  For comparison, `pop_count` scores 0.0589. The ml-32m floor is chosen from the ml-32m
  validation sweep (pending).
