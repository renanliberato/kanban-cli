Feature: Edit Card
  As a user
  I want to edit existing card titles
  So that I can correct mistakes or update task descriptions

  Scenario: Edit a card title and save with Enter
    Given a board with a card "old title" in "To Do"
    When I launch the application
    When I edit the card "old title" to "new title"
    Then the "To Do" column should contain "new title"
    And the "To Do" column should not contain "old title"

  Scenario: ESC cancels editing and keeps original title
    Given a board with a card "original" in "To Do"
    When I launch the application
    When I begin editing the card "original"
    When I change the title to "modified"
    When I press ESC
    Then the "To Do" column should contain "original"
    And the "To Do" column should not contain "modified"
