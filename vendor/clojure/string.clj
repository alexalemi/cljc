;; clojure.string — namespace shim over the flat str/* builtins, so that
;; (require '[clojure.string :as string]) (any alias, or fully-qualified
;; clojure.string/split) works. The conventional :as str alias hits the
;; flat builtins directly and never loads this file.

(ns clojure.string)

(def blank? str/blank?)
(def starts-with? str/starts-with?)
(def ends-with? str/ends-with?)
(def includes? str/includes?)
(def index-of str/index-of)
(def lower-case str/lower-case)
(def upper-case str/upper-case)
(def trim str/trim)
(def replace str/replace)
(def replace-first str/replace-first)
(def split str/split)
(def split-lines str/split-lines)
(def join str/join)
