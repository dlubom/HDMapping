# Szablon promptu kolejnego czatu

Agent uzupełnia wszystkie nawiasy danymi po zweryfikowaniu pushu. Dla nieukończonej karty nazwa i branch pozostają te same. Przy zakończonym P17 przygotuj prompt końcowego odbioru wyników zamiast nieistniejącej kolejnej karty.

```text
Pracujemy w moim forku https://github.com/dlubom/HDMapping.
Wykonaj wyłącznie [PXX — nazwa karty / kontynuacja karty].

Baza: branch [branch], zweryfikowany zdalny SHA [pełny SHA].
Branch do tej pracy: [mcp/pXX].
Poprzedni raport: [repozytoryjna ścieżka i link do raportu].
Stan przekazania: [co zaliczono, czego brakuje, dostępne dowody].

Sprawdź aktualny Git, remotes i zdalny head. Zachowaj istniejące zmiany.
Nowy branch utwórz od przekazanej bazy; istniejący branch sprawdź przed
kontynuacją. Przy nowszym headzie sprawdź różnice zamiast resetować historię.

Przeczytaj AGENTS.md, plans/mcp/README.md, status.md, design.md,
ograniczenia globalne i wskazaną kartę implementation-plan.md oraz raport
poprzednika. Zastosuj jego sprawdzone kontrakty i wersje; nie przeprojektowuj
uzgodnionego zakresu bez materialnej przyczyny.

[Konkretne kryteria bieżącej karty / brakujące kryteria kontynuacji.]

Wykonaj tylko tę kartę. Zlecam aktualizację raportu i statusu, właściwe
sprawdzenia, przegląd, commit oraz push na wskazany branch mojego forka.
Zweryfikuj zdalny SHA i opublikowane pliki; dla zmian kodu podaj wyniki
sprawdzeń dokładnie tego SHA. Samo in_progress nie oznacza sukcesu.
Bez otwierania PR, merge, release i pushu do upstream.

Na końcu podaj wynik, dowody, ograniczenia, linki i dokładny SHA.
Wygeneruj gotowy prompt NOWEGO CZATU według
plans/mcp/prompts/next-task-template.md: po pełnym odbiorze tylko następna
karta, przy brakach kontynuacja tej samej karty. Powtórz w nim zasadę
jednej karty, raportu, commit/pushu, kolejnego promptu i zatrzymania.
Zakończ; nie wykonuj kolejnej karty w tym czacie.
```
