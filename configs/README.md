# Model configs

One JSON file per model with its starting hyperparameters (spec defaults where the spec gives one).
Each file is copied into every artifact and run report. Tuned configs are added as
`<model>.<dataset>.json` once validation-set tuning has picked them; the test set is evaluated once
per final config.
