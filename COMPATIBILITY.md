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

## Partial

| Library | Status |
|---|---|
| org.clojure/core.match | loads; the `match` macro fails at expansion (`vector-pattern` internals) — needs bring-up |
| com.rpl/specter | loads with riddley vendored; `defnav`-generated vars missing at use — its macro layer needs bring-up |
| markdown-clj | parser namespaces load; `md-to-html-string` needs `java.io` BufferedReader/Writer shims — needs bring-up |
| meander | scalar/vector/logic-var matching works; map patterns WIP |

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
