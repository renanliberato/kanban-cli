Feature: Labels
  As a user
  I want to add and toggle labels on cards
  So that I can categorize and filter my tasks

  Background:
    Given a board with a card "task" in "To Do"

  Scenario: Add a label to a card via detail view
    When I launch the application
    When I press Enter
    When I press "l"
    When I press "n"
    When I type "bug"
    When I press Enter
    And I press ESC
    And I press ESC
    Then the "To Do" column should contain "task"

  Scenario: Labels persisted after restart
    When I launch the application
    When I press Enter
    When I press "l"
    When I press "n"
    When I type "feature"
    When I press Enter
    When I press ESC
    When I press ESC
    When I quit the application
    When I launch the application
    Then the "To Do" column should contain "task"
