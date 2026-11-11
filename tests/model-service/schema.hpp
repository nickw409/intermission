// A small JSON-Schema subset validator plus the response schemas (and the recommend/compare
// request schemas) of contracts/model.yaml.
//
// The repo has no YAML parser on the C++ side, so the component schemas are transcribed below as
// JSON. contract_fixtures_test.cpp guards against drift: every `required` list and `enum` list
// transcribed here must appear verbatim in model.yaml, the request properties must match, and
// every fixture response must validate.
//
// Supported keywords: $ref (to "#/components/schemas/<Name>"), type (string or array of strings),
// enum, required, properties, additionalProperties (false only), items, minItems, maxItems,
// uniqueItems, minimum, maximum, multipleOf.
#pragma once

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace recs::testing {

inline const nlohmann::json& model_components() {
  static const nlohmann::json c = nlohmann::json::parse(R"JSON({
    "Error": {"type": "object", "additionalProperties": false, "required": ["error"],
      "properties": {"error": {"type": "object", "additionalProperties": false, "required": ["code", "message"],
        "properties": {"code": {"type": "string", "enum": ["bad_request", "invalid_json", "body_too_large", "not_found", "unknown_model", "model_unavailable", "internal"]},
                       "message": {"type": "string"}}}}},
    "ModelName": {"type": "string", "enum": ["pop", "pop_count", "knn", "content", "mf", "als", "hybrid", "mmr"]},
    "RatingIn": {"type": "object", "additionalProperties": false, "required": ["movie_id", "rating"],
      "properties": {"movie_id": {"type": "integer", "minimum": 1, "maximum": 2147483647},
                     "rating": {"type": "number", "minimum": 0.5, "maximum": 5.0, "multipleOf": 0.5}}},
    "Ratings": {"type": "array", "maxItems": 5000, "items": {"$ref": "#/components/schemas/RatingIn"}},
    "Exclude": {"type": "array", "maxItems": 10000, "items": {"type": "integer", "minimum": 1, "maximum": 2147483647}},
    "Genre": {"type": "string", "enum": ["Action", "Adventure", "Animation", "Children", "Comedy", "Crime", "Documentary", "Drama",
      "Fantasy", "Film-Noir", "Horror", "IMAX", "Musical", "Mystery", "Romance", "Sci-Fi", "Thriller", "War", "Western"]},
    "Genres": {"type": "array", "maxItems": 19, "uniqueItems": true, "items": {"$ref": "#/components/schemas/Genre"}},
    "RecommendRequest": {"type": "object", "additionalProperties": false, "required": ["model", "ratings", "k"],
      "properties": {"model": {"$ref": "#/components/schemas/ModelName"}, "ratings": {"$ref": "#/components/schemas/Ratings"},
                     "k": {"type": "integer", "minimum": 1, "maximum": 100}, "exclude": {"$ref": "#/components/schemas/Exclude"},
                     "genres": {"$ref": "#/components/schemas/Genres"}}},
    "CompareRequest": {"type": "object", "additionalProperties": false, "required": ["models", "ratings", "k"],
      "properties": {"models": {"type": "array", "minItems": 1, "maxItems": 5, "uniqueItems": true, "items": {"$ref": "#/components/schemas/ModelName"}},
                     "ratings": {"$ref": "#/components/schemas/Ratings"},
                     "k": {"type": "integer", "minimum": 1, "maximum": 100}, "exclude": {"$ref": "#/components/schemas/Exclude"},
                     "genres": {"$ref": "#/components/schemas/Genres"}}},
    "Because": {"type": "object", "additionalProperties": false, "required": ["movie_id", "weight"],
      "properties": {"movie_id": {"type": "integer"}, "weight": {"type": "number"}}},
    "Item": {"type": "object", "additionalProperties": false, "required": ["movie_id", "score", "because"],
      "properties": {"movie_id": {"type": "integer"}, "score": {"type": "number"},
                     "because": {"type": "array", "maxItems": 3, "items": {"$ref": "#/components/schemas/Because"}}}},
    "RecommendResponse": {"type": "object", "additionalProperties": false, "required": ["model", "items", "ignored"],
      "properties": {"model": {"$ref": "#/components/schemas/ModelName"},
                     "items": {"type": "array", "items": {"$ref": "#/components/schemas/Item"}},
                     "ignored": {"type": "integer", "minimum": 0}}},
    "CompareResponse": {"type": "object", "additionalProperties": false, "required": ["results", "ignored"],
      "properties": {"results": {"type": "array", "items": {"type": "object", "additionalProperties": false, "required": ["model", "items"],
                       "properties": {"model": {"$ref": "#/components/schemas/ModelName"},
                                      "items": {"type": "array", "items": {"$ref": "#/components/schemas/Item"}}}}},
                     "ignored": {"type": "integer", "minimum": 0}}},
    "SimilarResponse": {"type": "object", "additionalProperties": false, "required": ["model", "movie_id", "items"],
      "properties": {"model": {"type": "string", "enum": ["knn", "content"]}, "movie_id": {"type": "integer"},
                     "items": {"type": "array", "items": {"type": "object", "additionalProperties": false, "required": ["movie_id", "similarity"],
                       "properties": {"movie_id": {"type": "integer"}, "similarity": {"type": "number"}}}}}},
    "IdList": {"type": "object", "additionalProperties": false, "required": ["movie_ids"],
      "properties": {"movie_ids": {"type": "array", "items": {"type": "integer"}}}},
    "MovieMeta": {"type": "object", "additionalProperties": false,
      "required": ["movie_id", "title", "year", "genres", "tmdb_id", "rating_count", "mean_rating"],
      "properties": {"movie_id": {"type": "integer"}, "title": {"type": "string"}, "year": {"type": ["integer", "null"]},
                     "genres": {"type": "array", "items": {"type": "string"}}, "tmdb_id": {"type": ["integer", "null"]},
                     "rating_count": {"type": "integer", "minimum": 0}, "mean_rating": {"type": ["number", "null"]}}},
    "BatchResponse": {"type": "object", "additionalProperties": false, "required": ["movies", "missing"],
      "properties": {"movies": {"type": "array", "items": {"$ref": "#/components/schemas/MovieMeta"}},
                     "missing": {"type": "array", "items": {"type": "integer"}}}},
    "ModelsResponse": {"type": "object", "additionalProperties": false, "required": ["artifact_version", "trained_at", "models", "default_model"],
      "properties": {"artifact_version": {"type": "string"}, "trained_at": {"type": "string"},
                     "default_model": {"$ref": "#/components/schemas/ModelName"},
                     "models": {"type": "array", "items": {"$ref": "#/components/schemas/ModelName"}}}},
    "Version": {"type": "object", "additionalProperties": false, "required": ["artifact_version"],
      "properties": {"artifact_version": {"type": "string"}}},
    "Health": {"type": "object", "additionalProperties": false, "required": ["status"],
      "properties": {"status": {"type": "string", "enum": ["ok", "unavailable"]}}}
  })JSON");
  return c;
}

namespace detail {

inline bool has_type(const nlohmann::json& v, const std::string& t) {
  if (t == "object") return v.is_object();
  if (t == "array") return v.is_array();
  if (t == "string") return v.is_string();
  if (t == "boolean") return v.is_boolean();
  if (t == "null") return v.is_null();
  if (t == "number") return v.is_number();
  if (t == "integer") {
    if (v.is_number_integer()) return true;
    return v.is_number_float() && std::isfinite(v.get<double>()) && std::floor(v.get<double>()) == v.get<double>();
  }
  return false;
}

inline void validate(const nlohmann::json& v, const nlohmann::json& schema, const std::string& at,
                     std::vector<std::string>& errors) {
  if (schema.contains("$ref")) {
    const std::string ref = schema["$ref"];
    const std::string prefix = "#/components/schemas/";
    validate(v, model_components().at(ref.substr(prefix.size())), at, errors);
    return;
  }
  if (schema.contains("type")) {
    bool ok = false;
    if (schema["type"].is_array()) {
      for (const auto& t : schema["type"]) ok = ok || has_type(v, t.get<std::string>());
    } else {
      ok = has_type(v, schema["type"].get<std::string>());
    }
    if (!ok) {
      errors.push_back(at + ": expected type " + schema["type"].dump() + ", got " + v.dump());
      return;
    }
  }
  if (schema.contains("enum") && std::find(schema["enum"].begin(), schema["enum"].end(), v) == schema["enum"].end()) {
    errors.push_back(at + ": " + v.dump() + " not in enum");
  }
  if (v.is_number()) {
    if (schema.contains("minimum") && v.get<double>() < schema["minimum"].get<double>()) errors.push_back(at + ": below minimum");
    if (schema.contains("maximum") && v.get<double>() > schema["maximum"].get<double>()) errors.push_back(at + ": above maximum");
    if (schema.contains("multipleOf")) {
      const double q = v.get<double>() / schema["multipleOf"].get<double>();
      if (std::floor(q) != q) errors.push_back(at + ": not a multiple of " + schema["multipleOf"].dump());
    }
  }
  if (v.is_object()) {
    if (schema.contains("required")) {
      for (const auto& r : schema["required"]) {
        if (!v.contains(r.get<std::string>())) errors.push_back(at + ": missing required " + r.get<std::string>());
      }
    }
    const auto props = schema.value("properties", nlohmann::json::object());
    for (const auto& [k, sub] : v.items()) {
      if (props.contains(k)) {
        validate(sub, props[k], at + "." + k, errors);
      } else if (schema.contains("additionalProperties") && schema["additionalProperties"] == false) {
        errors.push_back(at + ": unexpected property " + k);
      }
    }
  }
  if (v.is_array()) {
    if (schema.contains("minItems") && v.size() < schema["minItems"].get<std::size_t>()) errors.push_back(at + ": too few items");
    if (schema.contains("maxItems") && v.size() > schema["maxItems"].get<std::size_t>()) errors.push_back(at + ": too many items");
    if (schema.value("uniqueItems", false)) {
      for (std::size_t i = 0; i < v.size(); ++i) {
        for (std::size_t j = i + 1; j < v.size(); ++j) {
          if (v[i] == v[j]) errors.push_back(at + ": duplicate items");
        }
      }
    }
    if (schema.contains("items")) {
      for (std::size_t i = 0; i < v.size(); ++i) validate(v[i], schema["items"], at + "[" + std::to_string(i) + "]", errors);
    }
  }
}

}  // namespace detail

/// Validates `v` against component schema `name`. Returns the list of violations (empty = valid).
inline std::vector<std::string> validate_against(const nlohmann::json& v, const std::string& name) {
  std::vector<std::string> errors;
  detail::validate(v, model_components().at(name), "$", errors);
  return errors;
}

}  // namespace recs::testing
