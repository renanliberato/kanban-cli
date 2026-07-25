Feature: Startup
  As a user
  I want to see the board rendered when I launch the application
  So that I can understand the current state at a glance

  Scenario: Renders three column headers with counters
    Given a board with no cards
    When I launch the application
    Then the screen should show the headers "To Do", "Doing", and "Done"
    And the header for "To Do" should show "0"
    And the header for "Doing" should show "0"
    And the header for "Done" should show "0"

  Scenario: Renders headers with correct card counts
    Given a board with the following cards
      | title      | column |
      | buy milk   | To Do  |
      | walk dog   | To Do  |
      | write code | Doing  |
    When I launch the application
    Then the header for "To Do" should show "2"
    And the header for "Doing" should show "1"
    And the header for "Done" should show "0"

  Scenario: Loads empty board from nonexistent file
    Given the board file does not exist
    When I launch the application
    Then the screen should show an empty board
