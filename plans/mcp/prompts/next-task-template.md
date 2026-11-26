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
Aktualizację przekazanej bazy planu włącz przez fast-forward lub merge,
zachowując dotychczasową pracę karty.

Przeczytaj AGENTS.md, plans/mcp/README.md, quality.md, status.md, design.md,
ograniczenia globalne i wskazaną kartę implementation-plan.md oraz raport
poprzednika. Zastosuj jego sprawdzone kontrakty i wersje; nie przeprojektowuj
uzgodnionego zakresu bez materialnej przyczyny.

[Konkretne kryteria bieżącej karty / brakujące kryteria kontynuacji.]

Zastosuj kontrole i wytyczne Go/C++ z plans/mcp/quality.md właściwe dla
tej karty. W raporcie podaj zakres kontroli, narzędzia i wersje, polecenia,
badany SHA oraz PASS/FAIL/NOT RUN z dowodami. Brak wymaganego sprawdzenia
oznacza INCOMPLETE. P00 kwalifikuje narzędzia, P01 wdraża kontrole Go,
a P03 obejmuje nimi nowe moduły C++; dalsze karty utrzymują te wymagania.
Automatyczny odbiór platformowy od P01 opiera się na macierzy GitHub Actions
z quality.md. Ręczne próby na osobnych maszynach mają własny zakres odbioru.

Wykonaj tylko tę kartę. Zlecam aktualizację raportu i statusu, właściwe
sprawdzenia, przegląd, commit oraz push na wskazany branch mojego forka.
Zweryfikuj zdalny SHA i opublikowane pliki; dla zmian kodu podaj wyniki
sprawdzeń dokładnie tego SHA. Samo in_progress nie oznacza sukcesu.
Bez otwierania lub scalania PR, scalania do main, release i pushu do upstream.

Na końcu podaj wynik, dowody, ograniczenia, linki i dokładny SHA.
Wygeneruj gotowy prompt NOWEGO CZATU według
plans/mcp/prompts/next-task-template.md: po pełnym odbiorze tylko następna
karta, przy brakach kontynuacja tej samej karty. Powtórz w nim zasadę
jednej karty, wymaganych kontroli jakości, raportu, commit/pushu,
kolejnego promptu i zatrzymania oraz odwołanie do quality.md.
Zakończ; nie wykonuj kolejnej karty w tym czacie.
```
