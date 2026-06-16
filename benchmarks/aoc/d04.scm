; AoC 2017 day 4 — s7 Scheme port of p04.clj (same input, same algorithm)
(define (slurp path)
  (call-with-input-file path
    (lambda (port)
      (let loop ((cs '()))
        (let ((c (read-char port)))
          (if (eof-object? c) (list->string (reverse cs)) (loop (cons c cs))))))))

; split a string on a predicate (drops empty tokens)
(define (split-on str pred)
  (let ((n (string-length str)))
    (let loop ((i 0) (cur '()) (out '()))
      (if (= i n)
          (reverse (if (null? cur) out (cons (list->string (reverse cur)) out)))
          (let ((c (string-ref str i)))
            (if (pred c)
                (loop (+ i 1) '() (if (null? cur) out (cons (list->string (reverse cur)) out)))
                (loop (+ i 1) (cons c cur) out)))))))

(define (newline? c) (char=? c #\newline))
(define (space? c) (char-whitespace? c))

(define lines (split-on (slurp "input/04.txt") newline?))

; count distinct elements of a list using a hash-table
(define (distinct-count lst)
  (let ((h (make-hash-table)) (c 0))
    (for-each (lambda (x)
                (if (not (hash-table-ref h x))
                    (begin (set! c (+ c 1)) (hash-table-set! h x #t))))
              lst)
    c))

(define (no-dups? ws) (= (length ws) (distinct-count ws)))

(define (sort-chars w) (list->string (sort! (string->list w) char<?)))

(define (no-anagrams? ws) (no-dups? (map sort-chars ws)))

(define (count-valid pred)
  (let loop ((ls lines) (c 0))
    (if (null? ls) c
        (loop (cdr ls) (+ c (if (pred (split-on (car ls) space?)) 1 0))))))

(display "Answer1: ") (display (count-valid no-dups?)) (newline)
(display "Answer2: ") (display (count-valid no-anagrams?)) (newline)
