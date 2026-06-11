(println (last (sort (map (fn [i] (mod (* i 2654435761) 1000003)) (range 200000)))))
