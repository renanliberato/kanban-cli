Feature: Add Card
  As a user
  I want to add new cards to any column
  So that I can capture tasks on my board

  Scenario: Add a card by typing title and pressing Enter
    Given a board with no cards
    When I launch the application
    When I add a card "buy milk" to "To Do"
    Then the "To Do" column should contain "buy milk"

  Scenario: ESC cancels adding a card
    Given a board with a card "keepme" in "To Do"
    When I launch the application
    When I begin adding a card with title "should-not-appear"
    When I press ESC
    Then the "To Do" column should contain "keepme"
    And the "To Do" column should not contain "should-not-appear"

  Scenario: Empty title cancels adding a card
    Given a board with a card "keepme" in "To Do"
    When I launch the application
    When I press "a"
    When I press Enter
    Then the "To Do" column should contain exactly
      | title  |
      | keepme |

  Scenario: Add cards to all three columns
    Given a board with no cards
    When I launch the application
    When I add a card "todo-card" to "To Do"
    When I add a card "doing-card" to "Doing"
    When I add a card "done-card" to "Done"
    Then the "To Do" column should contain "todo-card"
    And the "Doing" column should contain "doing-card"
    And the "Done" column should contain "done-card"

  Scenario: Add a card in a non-default column via explicit navigation
    Given a board with no cards
    When I launch the application
    When I press "l"
    And I add a card "in-doing" to "Doing"
    Then the "Doing" column should contain "in-doing"
    And the "To Do" column should not contain "in-doing"
