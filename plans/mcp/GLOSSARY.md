# Przetwarzanie HDMapping

Pojęcia używane podczas planowania dostępu do przetwarzania przez MCP. Istniejące pojęcia sesji i projektu zachowują znaczenie z HDMapping.

## Pojęcia

**Nagranie Mandeye**:
Surowe dane z pomiaru urządzeniem Mandeye, stanowiące wejście do wyznaczania trajektorii i chmur punktów.
_Unikać_: projekt, sesja wynikowa

**Sesja HDMapping**:
Zbiór chmur punktów wraz z ich położeniami i powiązanymi danymi rejestracji, przetwarzany jako jedna całość.
_Unikać_: połączenie MCP, projekt wielosesyjny

**Projekt wielosesyjny**:
Zbiór sesji HDMapping wraz z relacjami służącymi ich wspólnej rejestracji.
_Unikać_: pojedyncza sesja

**Zadanie przetwarzania**:
Jedno uruchomienie obliczeń HDMapping z określonym wejściem i parametrami, które może trwać po zamknięciu asystenta i okna aplikacji. Jego postęp i wynik można odczytać po ponownym połączeniu.
_Unikać_: rozmowa, pojedyncze połączenie MCP

**Stan roboczy HDMapping**:
Aktualna postać danych i ustawień przetwarzania, na której wspólnie pracują użytkownik w GUI i asystent, obejmująca również niezapisane zmiany.
_Unikać_: wyłącznie plik na dysku, ustawienia widoku

**Wynik przetwarzania**:
Rezultat konkretnego zadania, zachowywany domyślnie osobno od wcześniejszej sesji lub projektu. Może odwoływać się do niezmienionych chmur współdzielonych z wejściem.
_Unikać_: automatyczne nadpisanie wejścia

**Samowystarczalny eksport**:
Kompletny zestaw danych wynikowych i wszystkich ich zależności, który można otworzyć bez dostępu do pierwotnych katalogów wejściowych.
_Unikać_: zwykły zapis projektu

**Kandydat więzu rejestracji**:
Proponowana relacja między wskazanymi skanami lub sesjami, która nie uczestniczy jeszcze w optymalizacji.
_Unikać_: aktywny więz

**Aktywny więz rejestracji**:
Relacja między skanami lub sesjami włączona do grafu używanego w optymalizacji.
_Unikać_: kandydat więzu

**Zakres operacji**:
Jawnie wskazany zbiór danych objętych przetwarzaniem lub eksportem, na przykład konkretne sesje, skany i przedziały trajektorii.
_Unikać_: domyślnie wszystko widoczne na ekranie
