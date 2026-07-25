Feature: Move Card
  As a user
  I want to move cards between columns
  So that I can track progress through my workflow

  Scenario: Move card right with 'L'
    Given a board with a card "task" in "To Do"
    When I launch the application
    When I move the card "task" right
    Then the "To Do" column should not contain "task"
    And the "Doing" column should contain "task"

  Scenario: Move card right to Done with 'L'
    Given a board with a card "task" in "Doing"
    When I launch the application
    When I move the card "task" right
    Then the "Doing" column should not contain "task"
    And the "Done" column should contain "task"

  Scenario: Move card left with 'H'
    Given a board with a card "task" in "Doing"
    When I launch the application
    When I move the card "task" left
    Then the "Doing" column should not contain "task"
    And the "To Do" column should contain "task"

  Scenario: Move card right with '>'
    Given a board with a card "task" in "To Do"
    When I launch the application
    When I move the card "task" right with ">"
    Then the "Doing" column should contain "task"
    And the "To Do" column should not contain "task"

  Scenario: Move card left with '<'
    Given a board with a card "task" in "Doing"
    When I launch the application
    When I move the card "task" left with "<"
    Then the "To Do" column should contain "task"
    And the "Doing" column should not contain "task"

  Scenario: Boundary no-op when moving leftmost card left
    Given a board with a card "stuck" in "To Do"
    When I launch the application
    When I try to move the leftmost card further left
    Then the "To Do" column should contain "stuck"

  Scenario: Boundary no-op when moving rightmost card right
    Given a board with a card "stuck" in "Done"
    When I launch the application
    # Navigate to the Done column first
    When I press "l"
    When I press "l"
    When I try to move the rightmost card further right
    Then the "Done" column should contain "stuck"
