Feature: AI enrich in TUI (M7a: direct-apply, no review screen)
  Scenario: Add a card and enrich with Ctrl+E applies description directly
    Given I launch the application with a fresh board
    When I add a card "Implement login" to "To Do"
    And I press Ctrl+E
    And I wait for the enrich job to complete
    Then the card "Implement login" should have a description

  Scenario: Enrich existing card applies description and labels
    Given a board with a card "Existing card" in "To Do"
    When I launch the application
    And I press Ctrl+E
    And I wait for the enrich job to complete
    Then the card "Existing card" should have a description

  Scenario: Flash hint after adding card
    Given I launch the application with a fresh board
    When I add a card "Hint test" to "To Do"
    Then the status bar should show "C-E"
