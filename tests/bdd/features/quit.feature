Feature: Quit
  As a user
  I want to quit the application cleanly from any state
  So that I can exit without losing data

  Scenario: Quit from default position exits 0
    Given a board with no cards
    When I launch the application
    When I quit the application
    Then the application should exit with code 0

  Scenario: Quit from a different column exits 0
    Given a board with the following cards
      | title  | column |
      | task-a | To Do  |
      | task-b | Doing  |
    When I launch the application
    When I press "l"
    When I quit the application
    Then the application should exit with code 0

  Scenario: Quit after adding a card persists the card
    Given a board with no cards
    When I launch the application
    When I add a card "quit-after-add" to "To Do"
    When I quit the application
    Then the application should exit with code 0
    And the "To Do" column should contain "quit-after-add"

  Scenario: Quit after editing a card persists the edit
    Given a board with a card "old-title" in "To Do"
    When I launch the application
    When I edit the card "old-title" to "new-title"
    When I quit the application
    Then the application should exit with code 0
    And the "To Do" column should contain "new-title"
    And the "To Do" column should not contain "old-title"
