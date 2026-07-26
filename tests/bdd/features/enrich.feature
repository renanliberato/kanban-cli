Feature: AI enrich in TUI with human review
  Scenario: Add a card and enrich with Ctrl+E
    Given I launch the application with a fresh board
    When I add a card "Implement login" to "To Do"
    And I press Ctrl+E
    And I wait for the enrich job to complete
    Then a review screen should appear

  Scenario: Cancel enrichment review
    Given I launch the application with a fresh board
    When I add a card "Cancel this" to "To Do"
    And I press Ctrl+E
    And I wait for the enrich job to complete
    When I cancel the enrichment review
    Then the card "Cancel this" should not have a description

  Scenario: Enrich existing card
    Given a board with a card "Existing card" in "To Do"
    When I launch the application
    And I press Ctrl+E
    And I wait for the enrich job to complete
    Then a review screen should appear

  Scenario: Flash hint after adding card
    Given I launch the application with a fresh board
    When I add a card "Hint test" to "To Do"
    Then the status bar should show "C-E"
