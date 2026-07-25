Feature: Delete Card
  As a user
  I want to delete cards from the board
  So that I can remove completed or irrelevant tasks

  Scenario: Confirm deletion with 'y'
    Given a board with the following cards
      | title       | column |
      | groceries   | To Do  |
      | walk dog    | To Do  |
    When I launch the application
    When I delete the card "groceries"
    And I confirm the deletion
    Then the "To Do" column should contain "walk dog"
    And the "To Do" column should not contain "groceries"

  Scenario: Cancel deletion with 'n'
    Given a board with a card "keepme" in "To Do"
    When I launch the application
    When I delete the card "keepme"
    And I cancel the deletion
    Then the "To Do" column should contain "keepme"

  Scenario: Cancel deletion with another key
    Given a board with a card "keepme" in "To Do"
    When I launch the application
    When I delete the card "keepme"
    And I dismiss the deletion with another key
    Then the "To Do" column should contain "keepme"
