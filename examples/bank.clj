;; bank.clj — mutable state with atoms, plus error handling.
;;
;; Showcases: atoms (the one mutable cell cljc offers), swap!/reset!,
;; ex-info/try/catch for domain errors, and validating updates inside swap!
;; so the state can never go negative.

;; The whole bank is one atom holding an immutable map of account -> balance.
;; Every "mutation" is swap! applying a pure function to that map.
(def accounts (atom {:alice 100 :bob 50}))

(defn deposit! [acct amount]
  (swap! accounts update acct (fnil + 0) amount))

;; A guarded withdrawal: the update fn throws if funds are short. Because
;; swap! runs the fn on the current value, the throw propagates out and the
;; atom is left untouched — no partial update.
(defn withdraw! [acct amount]
  (swap! accounts
         (fn [m]
           (let [bal (get m acct 0)]
             (if (< bal amount)
               (throw (ex-info "Insufficient funds"
                               {:account acct :balance bal :requested amount}))
               (update m acct - amount))))))

(defn transfer! [from to amount]
  (withdraw! from amount)
  (deposit! to amount))

(defn show []
  (doseq [[acct bal] (sort @accounts)]
    (println (format "  %-8s $%d" (name acct) bal))))

(println "Opening balances:")
(show)

(println "\nalice deposits $25, transfers $40 to bob:")
(deposit! :alice 25)
(transfer! :alice :bob 40)
(show)

;; Demonstrate the error path: bob tries to overdraw. We catch the ex-info,
;; read its data, and confirm the balances were NOT corrupted.
(println "\nbob tries to withdraw $1000:")
(try
  (withdraw! :bob 1000)
  (catch Exception e
    (let [{:keys [balance requested]} (ex-data e)]
      (println (format "  Rejected: %s (had $%d, wanted $%d)"
                       (ex-message e) balance requested)))))

(println "\nFinal balances (unchanged by the failed withdrawal):")
(show)
