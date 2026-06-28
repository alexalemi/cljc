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
(def triml str/triml)
(def trimr str/trimr)
(def trim-newline str/trim-newline)
(def capitalize str/capitalize)
(def reverse str/reverse)
(def last-index-of str/last-index-of)
(def escape str/escape)
(def re-quote-replacement str/re-quote-replacement)
