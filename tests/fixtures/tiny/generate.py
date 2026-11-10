#!/usr/bin/env python3
"""Generates the `tiny` MovieLens-format fixture dataset (committed; rerun only to change it).

Shape: 20 users, 30 movies. Deliberate edge cases:
  - titles with quoted commas and escaped quotes, accented UTF-8, a title with no year
  - one movie with "(no genres listed)", one movie with no ratings at all (cold item, id 30)
  - links.csv rows with an empty tmdbId
  - users 19 and 20 have fewer than 10 ratings (dropped by the split)
  - user 3 has several ratings with the same timestamp (split tie-breaking)
  - user 7's last ratings include movie 29, which nobody else rates (a cold test item)
Deterministic: a fixed LCG, no randomness from the environment.
"""
import csv, pathlib

OUT = pathlib.Path(__file__).resolve().parent

MOVIES = [
    (1, "Toy Story (1995)", "Adventure|Animation|Children|Comedy|Fantasy", "0114709", "862"),
    (2, "Jumanji (1995)", "Adventure|Children|Fantasy", "0113497", "8844"),
    (3, "Heat (1995)", "Action|Crime|Thriller", "0113277", "949"),
    (4, "American President, The (1995)", "Comedy|Drama|Romance", "0112346", "9087"),
    (5, "City of Lost Children, The (Cité des enfants perdus, La) (1995)", "Adventure|Drama|Fantasy|Mystery|Sci-Fi", "0112682", "902"),
    (6, "Usual Suspects, The (1995)", "Crime|Mystery|Thriller", "0114814", "629"),
    (7, "Braveheart (1995)", "Action|Drama|War", "0112573", "197"),
    (8, "Apollo 13 (1995)", "Adventure|Drama|IMAX", "0112384", "568"),
    (9, "Star Wars: Episode IV - A New Hope (1977)", "Action|Adventure|Sci-Fi", "0076759", "11"),
    (10, "Pulp Fiction (1994)", "Comedy|Crime|Drama|Thriller", "0110912", "680"),
    (11, "Shawshank Redemption, The (1994)", "Crime|Drama", "0111161", "278"),
    (12, "Forrest Gump (1994)", "Comedy|Drama|Romance|War", "0109830", "13"),
    (13, "Lion King, The (1994)", "Adventure|Animation|Children|Drama|Musical|IMAX", "0110357", "8587"),
    (14, "Jurassic Park (1993)", "Action|Adventure|Sci-Fi|Thriller", "0107290", "329"),
    (15, "Silence of the Lambs, The (1991)", "Crime|Horror|Thriller", "0102926", "274"),
    (16, "Matrix, The (1999)", "Action|Sci-Fi|Thriller", "0133093", "603"),
    (17, "Fargo (1996)", "Comedy|Crime|Drama|Thriller", "0116282", "275"),
    (18, "Amélie (Fabuleux destin d'Amélie Poulain, Le) (2001)", "Comedy|Romance", "0211915", "194"),
    (19, "Lord of the Rings: The Fellowship of the Ring, The (2001)", "Adventure|Fantasy", "0120737", "120"),
    (20, "Spirited Away (Sen to Chihiro no kamikakushi) (2001)", "Adventure|Animation|Fantasy", "0245429", "129"),
    (21, "Godfather, The (1972)", "Crime|Drama", "0068646", "238"),
    (22, "Alien (1979)", "Horror|Sci-Fi", "0078748", "348"),
    (23, "Toy Story 2 (1999)", "Adventure|Animation|Children|Comedy|Fantasy", "0120363", "863"),
    (24, "Finding Nemo (2003)", "Adventure|Animation|Children|Comedy", "0266543", "12"),
    (25, "Notebook, The (2004)", "Drama|Romance", "0332280", "11036"),
    (26, '"Great Performances" Cats (1998)', "Musical", "0173714", ""),
    (27, "Hamlet, Prince of Denmark", "Drama", "", ""),
    (28, "Pirates of the Caribbean: The Curse of the Black Pearl (2003)", "(no genres listed)", "0325980", "22"),
    (29, "Obscure Festival Short (2019)", "Documentary", "", ""),
    (30, "Never Rated (2020)", "Drama", "", ""),
]

class Lcg:
    def __init__(self, seed): self.s = seed
    def next(self):
        self.s = (self.s * 6364136223846793005 + 1442695040888963407) % (1 << 64)
        return self.s >> 33
    def below(self, n): return self.next() % n

def main():
    rng = Lcg(20261110)
    base_ts = 1_600_000_000
    rateable = [m[0] for m in MOVIES if m[0] not in (29, 30)]
    ratings = []
    for user in range(1, 21):
        n = 6 if user in (19, 20) else 10 + rng.below(9)  # 10..18 ratings
        pool = rateable[:]
        chosen = []
        while len(chosen) < n:
            chosen.append(pool.pop(rng.below(len(pool))))
        ts = base_ts + user * 100_000
        for i, movie in enumerate(chosen):
            rating = (1 + rng.below(10)) / 2.0  # 0.5 .. 5.0 in 0.5 steps
            if user == 3 and i >= n - 4:
                pass  # last four share a timestamp
            else:
                ts += 1000 + rng.below(5000)
            ratings.append((user, movie, rating, ts))
        if user == 7:
            ratings.append((user, 29, 4.5, ts + 10_000))  # cold test item: last rating, nobody else rates 29

    with open(OUT / "movies.csv", "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(["movieId", "title", "genres"])
        for m in MOVIES:
            w.writerow([m[0], m[1], m[2]])
    with open(OUT / "links.csv", "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(["movieId", "imdbId", "tmdbId"])
        for m in MOVIES:
            w.writerow([m[0], m[3], m[4]])
    with open(OUT / "ratings.csv", "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(["userId", "movieId", "rating", "timestamp"])
        for r in ratings:
            w.writerow([r[0], r[1], f"{r[2]:.1f}", r[3]])
    tags = [
        (1, 1, "pixar", base_ts + 1), (2, 1, "Pixar", base_ts + 2), (1, 23, "pixar", base_ts + 3),
        (4, 16, "cyberpunk", base_ts + 4), (5, 16, "philosophy", base_ts + 5), (4, 9, "space opera", base_ts + 6),
        (6, 10, "nonlinear", base_ts + 7), (6, 10, "quotable, violent", base_ts + 8),
        (7, 19, "epic", base_ts + 9), (8, 20, "studio ghibli", base_ts + 10), (99, 22, "creepy", base_ts + 11),
    ]
    with open(OUT / "tags.csv", "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(["userId", "movieId", "tag", "timestamp"])
        for t in tags:
            w.writerow(t)
    print(f"{len(ratings)} ratings, {len(MOVIES)} movies, {len(tags)} tags")

if __name__ == "__main__":
    main()
