# Library compatibility

Tested with `cljc vendor` / `(vendor! …)` on 2026-07-07 (v0.2.0+). "Works"
means a functional smoke test passed, not just loading. Transitive
dependencies are not auto-resolved — vendor them too (listed where needed).

## Works — verified functionally

| Library | Verified |
|---|---|
| org.clojure/test.check | `quick-check` runs properties end-to-end |
| org.clojure/tools.cli | `parse-opts` with `:parse-fn` |
| org.clojure/data.csv | `read-csv` |
| org.clojure/math.combinatorics | `combinations`, `permutations` |
| org.clojure/data.json (vendored source) | full read/write round-trip incl. unicode |
| dev.weavejester/medley | `map-vals` |
| weavejester/integrant (+ weavejester/dependency) | `init` with `defmethod init-key` |
| borkdude/edamame (+ org.clojure/tools.reader) | `parse-string` |
| environ/environ | `environ.core` loads (use the Clojars coord, not the multi-project git repo) |

## Works — loads clean, exercised by the test suite / prior bring-up

emmy (CAS), datascript, core.logic, core.async (vendored port), hiccup,
instaparse, camel-snake-kebab, clj-yaml, cheshire (vendored port),
nextjournal.markdown, clojure.tools.logging, clojure.data.priority-map,
stuartsierra/component, weavejester/dependency, rewrite-clj (all namespaces
load; zip API untested), org.clojure/tools.reader, riddley,
borkdude/deflet.

## Works — verified functionally (bring-up round, 2026-07-07)

| Library | Verified |
|---|---|
| org.clojure/core.match | wildcards, multi-column, nested vectors, maps, seq patterns, `:or`, `:guard`, `:as`, rest patterns, clause ordering (13/13 battery) |
| markdown-clj | `md-to-html-string`: headings, emphasis, lists, links, code blocks, tables, YAML front-matter metadata |
| meander/epsilon | `match`/`search`/`find`: logic + memory vars, maps (nested/multi-key), `scan`, `pred`, `or`, `app` (9/9 battery). `m/rewrite` not yet: meander registers its `with`/`$` parsers under syntax-quoted bare symbols, which cljc's syntax-quote leaves unqualified (documented divergence) while lookups use the qualified name |

## Works — verified functionally (bring-up round, 2026-08-27)

| Library | Verified |
|---|---|
| com.rpl/specter (+ riddley/riddley) | `select`/`transform`/`setval`/`select-one`/`select-first`/`select-any`/`vtransform`/`replace-in`/`traverse-all`, `defnav`/`defdynamicnav`/`recursive-path`/`path`, navigators `ALL MAP-VALS MAP-KEYS FIRST LAST END BEGINNING BEFORE-ELEM AFTER-ELEM NONE-ELEM INDEXED-VALS ATOM META NAME NAMESPACE VAL DISPENSE STOP STAY`, `keypath nthpath srange filterer walker submap subset must pred pred= pred> view parser putval collect-one if-path cond-path multi-path selected? not-selected? transformed terminal terminal-val nil->val regex-nav map-key index-nav before-index with-fresh-collected continue-then-stay stay-then-continue`, `NONE` removal (92/92 battery). Runs specter's `:bb` reader-conditional branches |

## Partial

| Library | Status |
|---|---|
| meander `m/rewrite` | its self-hosted clause analyzer reports non-exhaustive — match/search/find are fine |

## Won't work (JVM-native by design)

Anything wrapping Java libraries directly: clj-http (Apache HTTP),
data.xml (SAX), next.jdbc, tools.namespace (java.io scanning), aleph/netty,
… Use cljc's batteries (`http.clj`, `json.clj`, `fs.clj`, `process.clj`)
for those needs.

## Notes

- `org.clojure/*` coordinates resolve via Maven Central automatically;
  everything else tries Clojars first, then GitHub.
- A namespace that fails only because a sibling loads later alphabetically
  is retried automatically; failures reported are real.
- File issues with the smallest failing `(require …)` — the error usually
  names the exact missing shim.
