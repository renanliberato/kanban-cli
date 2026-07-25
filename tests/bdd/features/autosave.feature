Feature: Autosave
  As a user
  I want my changes to be saved to disk immediately after each operation
  So that I never lose work even if the application crashes

  Scenario: Card added is saved to disk before quitting
    Given a board with no cards
    When I launch the application
    When I add a card "immediate-save" to "To Do"
    Then the board file on disk should contain "immediate-save" in "To Do"

  Scenario: Card deleted is removed from disk immediately
    Given a board with a card "delete-me" in "To Do"
    When I launch the application
    When I delete the card "delete-me"
    And I confirm the deletion
    Then the board file on disk should no longer contain "delete-me" in "To Do"

  Scenario: Card edited is updated on disk immediately
    Given a board with a card "before" in "To Do"
    When I launch the application
    When I edit the card "before" to "after"
    Then the board file on disk should contain "after" in "To Do"
    And the board file on disk should no longer contain "before" in "To Do"

  Scenario: Card moved is reflected on disk immediately
    Given a board with a card "mover" in "To Do"
    When I launch the application
    When I move the card "mover" right
    Then the board file on disk should no longer contain "mover" in "To Do"
    And the board file on disk should contain "mover" in "Doing"
