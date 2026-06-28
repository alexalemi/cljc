(ns clojure.datafy
  "Minimal datafy/nav: identity unless a value carries the Datafiable/Navigable
   protocols (cljc has no metadata-protocol dispatch, so these are basic).")
(defn datafy [x] x)
(defn nav [_coll _k v] v)
