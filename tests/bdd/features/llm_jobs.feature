Feature: LLM job subsystem
  Background:
    Given a board with a card "Test title" in "To Do"

  Scenario: Submit an enrich job via Ctrl+E and see status bar update
    When I launch the application
    And I press Ctrl+E
    Then the status bar should show "Enriching"

  Scenario: TUI stays responsive while LLM job is running
    When I launch the application
    And I press Ctrl+E
    Then I can still navigate with "j"

  Scenario: Job completes and review screen appears
    When I launch the application
    And I press Ctrl+E
    And I wait for the enrich job to complete
    Then a review screen should appear
