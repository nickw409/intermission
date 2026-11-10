# Model configs

One JSON file per model with its starting hyperparameters (spec defaults where the spec gives one).
Each file is copied into every artifact and run report. Tuned configs are added as
`<model>.<dataset>.tuned.json` once validation-set tuning has picked them; `recs-train train`
uses that file for the dataset when it exists, else `<model>.json`; the test set is evaluated once
per final config.
