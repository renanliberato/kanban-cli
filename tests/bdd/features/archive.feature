Feature: Archive
  As a user
  I want to archive completed cards instead of deleting them
  So that I can clean up my board without losing history

  Scenario: Archiving a card hides it from the board
    Given a board with a card "done-task" in "Done"
    When I launch the application
    When I press "l" twice
    When I reset the screen buffer
    When I press "x"
    Then the screen should not show "done-task"

  Scenario: Ctrl+A toggles archived card visibility
    Given a board with a card "done-task" in "Done"
    When I launch the application
    When I press "l" twice
    When I reset the screen buffer
    When I press "x"
    Then the screen should not show "done-task"
    When I press Ctrl+A
    Then the screen should show "done-task"

  Scenario: CLI list excludes archived by default
    Given an empty board file at the default path
    Given I have added card "visible" in "Done"
    When I launch the application
    When I press "l" twice
    When I press "x"
    When I quit the application
    When I run "list" with no arguments
    Then the output should not contain "visible"

  Scenario: CLI list --archived shows archived cards
    Given an empty board file at the default path
    Given I have added card "archived-card" in "Done"
    When I launch the application
    When I press "l" twice
    When I press "x"
    When I quit the application
    When I run "list" with arguments "--archived"
    Then the output should contain "archived-card"
